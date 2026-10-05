"""One selection and retrieval, then one of two explicit grounded responses."""

from copy import deepcopy
from dataclasses import dataclass
from enum import Enum
import json
from pathlib import Path
from typing import Any, Protocol

from .diagnostic_provider import ProviderError, ProviderTimeoutError
from .diagnostic_runner import DiagnosticRunResult, RunStatus, run_diagnosis
from .evidence_tool_call import (
    EvidenceToolCall, HISTORICAL_TOOL_NAME, ToolCallError,
    question_target, validate_tool_call,
)
from .historical_order_tool import DEFAULT_HISTORICAL_EVIDENCE_PATH, get_historical_order_evidence
from .historical_summary_runner import HistoricalSummaryRunResult, run_historical_summary
from .partial_fill_tool import (
    DEFAULT_EVIDENCE_PATH, get_partial_fill_evidence,
)


class ToolDiagnosticProvider(Protocol):
    def select_evidence_tool(self, question: str) -> dict[str, Any]:
        """Return an untrusted assistant message, not a parsed diagnosis."""
        ...

    def complete_with_evidence_tool(
        self, question: str, call: EvidenceToolCall, evidence_json: str, prompt: str,
    ) -> str:
        """Return untrusted final diagnosis text after supplying the tool result."""
        ...


@dataclass(frozen=True)
class FakeToolDiagnosticProvider:
    selection: dict[str, Any]
    raw_output: str = ""
    selection_failure: ProviderError | None = None
    diagnosis_failure: ProviderError | None = None

    def select_evidence_tool(self, question: str) -> dict[str, Any]:
        if self.selection_failure is not None:
            raise self.selection_failure
        return deepcopy(self.selection)

    def complete_with_evidence_tool(
        self, question: str, call: EvidenceToolCall, evidence_json: str, prompt: str,
    ) -> str:
        if self.diagnosis_failure is not None:
            raise self.diagnosis_failure
        return self.raw_output


class ToolRunStatus(str, Enum):
    TOOL_CALL_INVALID = "tool_call_invalid"
    TOOL_EXECUTION_ERROR = "tool_execution_error"
    TOOL_NOT_REQUESTED = "tool_not_requested"


@dataclass(frozen=True)
class ToolDiagnosticRunResult:
    status: RunStatus | ToolRunStatus
    selected_tool: EvidenceToolCall | None = None
    diagnosis_run: DiagnosticRunResult | None = None
    historical_run: HistoricalSummaryRunResult | None = None
    tool_error: dict[str, Any] | None = None

    def to_dict(self) -> dict[str, Any]:
        result = (self.diagnosis_run.to_dict() if self.diagnosis_run else {
            "status": self.status.value, "diagnosis": None,
            "errors": [], "raw_provider_output": None,
        })
        if self.historical_run:
            result = self.historical_run.to_dict()
        elif self.selected_tool and self.selected_tool.name == HISTORICAL_TOOL_NAME:
            result = {"status": self.status.value, "historical_summary": None,
                      "errors": [], "raw_provider_output": None}
        result["selected_tool"] = self.selected_tool.to_dict() if self.selected_tool else None
        result["tool_error"] = self.tool_error
        return result


@dataclass(frozen=True)
class _ToolCompletion:
    provider: ToolDiagnosticProvider
    question: str
    call: EvidenceToolCall
    evidence_json: str

    def complete(self, prompt: str) -> str:
        return self.provider.complete_with_evidence_tool(
            self.question, self.call, self.evidence_json, prompt,
        )


def run_tool_diagnosis(
    question: str, provider: ToolDiagnosticProvider,
    *, evidence_path: Path = DEFAULT_EVIDENCE_PATH,
    evidence_source: str = "fixture",
    historical_evidence_path: Path = DEFAULT_HISTORICAL_EVIDENCE_PATH,
) -> ToolDiagnosticRunResult:
    try:
        requested_order, expected_tool = question_target(question)
    except ToolCallError as error:
        return ToolDiagnosticRunResult(
            ToolRunStatus.TOOL_CALL_INVALID,
            tool_error={"error": "unsupported_question", "message": str(error)},
        )
    try:
        selection = provider.select_evidence_tool(question)
    except ProviderTimeoutError:
        return ToolDiagnosticRunResult(RunStatus.PROVIDER_TIMEOUT)
    except ProviderError:
        return ToolDiagnosticRunResult(RunStatus.PROVIDER_ERROR)
    try:
        call = validate_tool_call(selection, requested_order, expected_tool)
    except ToolCallError as error:
        return ToolDiagnosticRunResult(
            ToolRunStatus.TOOL_CALL_INVALID,
            tool_error={"error": "invalid_tool_call", "message": str(error)},
        )
    if call is None:
        return ToolDiagnosticRunResult(
            ToolRunStatus.TOOL_NOT_REQUESTED,
            tool_error={"error": "tool_not_requested"},
        )
    if call.name == HISTORICAL_TOOL_NAME:
        evidence = get_historical_order_evidence(
            call.order_id, evidence_path=historical_evidence_path, evidence_source=evidence_source,
        )
    else:
        evidence = get_partial_fill_evidence(
            call.order_id, evidence_path=evidence_path, evidence_source=evidence_source,
        )
    if "error" in evidence:
        return ToolDiagnosticRunResult(
            ToolRunStatus.TOOL_EXECUTION_ERROR, selected_tool=call, tool_error=evidence,
        )
    evidence_json = json.dumps(evidence, sort_keys=True, ensure_ascii=False)
    if call.name == HISTORICAL_TOOL_NAME:
        historical_run = run_historical_summary(
            evidence_json, _ToolCompletion(provider, question, call, evidence_json),
        )
        return ToolDiagnosticRunResult(
            historical_run.status, selected_tool=call, historical_run=historical_run,
        )
    diagnosis_run = run_diagnosis(
        evidence_json, _ToolCompletion(provider, question, call, evidence_json),
    )
    return ToolDiagnosticRunResult(
        diagnosis_run.status, selected_tool=call, diagnosis_run=diagnosis_run,
    )
