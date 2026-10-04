"""Synchronous, one-call orchestration of D1-A parsing and grounding."""

from dataclasses import asdict, dataclass
from enum import Enum
from typing import Any

from .diagnostic_provider import (
    DiagnosticProvider, ProviderError, ProviderTimeoutError,
)
from .partial_fill_diagnosis import (
    Cause, ErrorCategory, PartialFillDiagnosis, ReferenceKind, RemainingState,
    ValidationError, ValidationResult, _load_json, parse_diagnosis_json,
    validate_diagnosis,
)


class RunStatus(str, Enum):
    ACCEPTED = "accepted"
    PROVIDER_TIMEOUT = "provider_timeout"
    PROVIDER_ERROR = "provider_error"
    PARSE_ERROR = "parse_error"
    VALIDATION_REJECTED = "validation_rejected"


@dataclass(frozen=True)
class DiagnosticRunResult:
    status: RunStatus
    # Only accepted results expose a diagnosis. Raw rejected text is diagnostic
    # material, not trusted claims. Summary prose is never fact-checked.
    diagnosis: PartialFillDiagnosis | None = None
    errors: tuple[ValidationError, ...] = ()
    raw_provider_output: str | None = None

    def to_dict(self) -> dict[str, Any]:
        return {
            "status": self.status.value,
            "diagnosis": asdict(self.diagnosis) if self.diagnosis else None,
            "errors": ValidationResult(self.errors).to_dict()["errors"],
            "raw_provider_output": self.raw_provider_output,
        }


def build_partial_fill_prompt(evidence_json: str) -> str:
    """Describe the existing closed schema and include the supplied D0 text."""
    causes = ", ".join(cause.value for cause in Cause)
    states = ", ".join(state.value for state in RemainingState)
    kinds = ", ".join(kind.value for kind in ReferenceKind)
    return f"""You diagnose only a partial_fill case.
Use only the supplied evidence. Do not invent facts.
Return only the required structured JSON diagnosis, without markdown or extra fields.
If the evidence does not support the requested cause, use unsupported_by_evidence.
That value abstains from a cause claim; all facts and references must still be grounded.

Required JSON schema:
Root keys: diagnosis_type, cause, summary, facts, evidence_refs.
diagnosis_type: exactly "partial_fill".
cause: one of {causes}.
summary: a nonempty display string, not a substitute for typed facts.
facts keys: order_id, submitted_quantity, matched_quantity, remaining_quantity,
limit_price, remaining_state. No extra keys.
order_id, submitted_quantity, limit_price: positive integers.
matched_quantity, remaining_quantity: nonnegative integers. No floats or booleans.
remaining_state: one of {states}, supported by evidence.
evidence_refs: a nonempty array of objects with exactly kind and id.
kind: one of {kinds}; id: a positive integer in that reference's own ID space.

Ground facts in target_order, matched_quantity, and post_execution.
A resting claim requires post_execution.resting = true; absence alone does not prove filled/cancelled.
insufficient_executable_liquidity requires matched_quantity < submitted_quantity,
pre_execution_book.total_executable_quantity = matched_quantity,
remaining_quantity > 0, and submitted_quantity = matched_quantity + remaining_quantity.
Order references must appear as order identities, Ledger references in trades.ledger_sequence,
and WAL references in provenance.wal_sequence. Do not use RequestId as OrderId.

Supplied D0 evidence JSON:
{evidence_json}
"""


def run_diagnosis(evidence_json: str, provider: DiagnosticProvider) -> DiagnosticRunResult:
    # Reuse D1-A's strict decoder, including duplicate-key/NaN rejection.
    # This is an input check, not a second implementation of evidence grounding.
    try:
        evidence = _load_json(evidence_json)
        if not isinstance(evidence, dict) or evidence.get("case_type") != "partial_fill":
            raise ValueError("Expected a D0 partial_fill evidence object")
    except (ValueError, TypeError) as error:
        return DiagnosticRunResult(
            RunStatus.VALIDATION_REJECTED,
            errors=(ValidationError(ErrorCategory.SCHEMA_ERROR, "evidence", str(error)),),
        )

    prompt = build_partial_fill_prompt(evidence_json)
    try:
        raw = provider.complete(prompt)
    except ProviderTimeoutError:
        return DiagnosticRunResult(RunStatus.PROVIDER_TIMEOUT)
    except ProviderError:
        return DiagnosticRunResult(RunStatus.PROVIDER_ERROR)

    parsed = parse_diagnosis_json(raw)
    if parsed.errors:
        return DiagnosticRunResult(
            RunStatus.PARSE_ERROR, errors=parsed.errors, raw_provider_output=raw,
        )
    assert parsed.diagnosis is not None
    validation = validate_diagnosis(parsed.diagnosis, evidence)
    if not validation.valid:
        return DiagnosticRunResult(
            RunStatus.VALIDATION_REJECTED, errors=validation.errors,
            raw_provider_output=raw,
        )
    return DiagnosticRunResult(
        RunStatus.ACCEPTED, diagnosis=parsed.diagnosis, raw_provider_output=raw,
    )
