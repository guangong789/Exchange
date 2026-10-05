"""One completion parsed and grounded exclusively as a historical summary."""

from dataclasses import asdict, dataclass, replace
from typing import Any

from .diagnostic_provider import DiagnosticProvider, ProviderError, ProviderTimeoutError
from .diagnostic_runner import RunStatus
from .historical_order_evidence_source import validate_historical_evidence_contract
from .historical_order_summary import (
    HistoricalOrderSummary, factual_historical_summary,
    parse_historical_summary_json, validate_historical_summary,
)
from .historical_replay_result import CANCEL_RESULTS, ReplayResult, SUBMIT_RESULTS
from .partial_fill_diagnosis import (
    ErrorCategory, ValidationError, ValidationResult, _load_json,
)


@dataclass(frozen=True)
class HistoricalSummaryRunResult:
    status: RunStatus
    historical_summary: HistoricalOrderSummary | None = None
    errors: tuple[ValidationError, ...] = ()
    raw_provider_output: str | None = None

    def to_dict(self) -> dict[str, Any]:
        return {
            "status": self.status.value,
            "historical_summary": asdict(self.historical_summary) if self.historical_summary else None,
            "errors": ValidationResult(self.errors).to_dict()["errors"],
            "raw_provider_output": self.raw_provider_output,
        }


def build_historical_summary_prompt(evidence_json: str) -> str:
    submit_names = ", ".join(result.value for result in ReplayResult if result in SUBMIT_RESULTS)
    cancel_names = ", ".join(result.value for result in ReplayResult if result in CANCEL_RESULTS)
    return f"""Summarize only durable historical facts in the supplied evidence.
Return only JSON without markdown or extra fields. No diagnosis cause is supported.
Required root keys: response_type, summary, facts, cancel_attempts, evidence_refs.
response_type: exactly "historical_order_summary".
summary: nonempty, conservative factual prose. Do not explain why matching stopped,
claim original response delivery, or imply successful execution from WAL admission alone.
The runtime renders accepted display prose from validated typed facts.
facts keys: order_id, submitted_quantity, submitted_price, submitted_side,
submission_recovered_result, matched_quantity, final_status, remaining_quantity,
outcome_basis, recovered_terminal_wal_sequence, torn_tail_ignored. No extra keys.
order_id, submitted_quantity, submitted_price: positive integers.
submitted_side: BUY or SELL. matched_quantity: nonnegative integer.
submission_recovered_result: exactly submission.recovered_result.
Allowed submit results: {submit_names}.
outcome_basis: exactly "deterministic_replay", copied from outcome_basis.
recovered_terminal_wal_sequence: positive integer from recovery.through_wal_sequence.
torn_tail_ignored: boolean from recovery.ignored_torn_tail, exactly true or false.
final_status: resting, fully_filled, or not_resting, exactly as final_state.status.
remaining_quantity: nonnegative integer or null, exactly as final_state.remaining_quantity.
No floats or booleans in integer fields. Null means unavailable; never invent a terminal reason.
Ground submission fields in submission.quantity, submission.price and submission.side.
cancel_attempts: an array representing EVERY evidence.cancel_attempts entry in the same WAL order.
Each object has exactly wal_sequence, account_id, request_id, recovered_result, copied exactly.
Allowed cancel results: {cancel_names}.
Do not omit, reorder, duplicate, or invent attempts. Use [] if evidence has no attempts.
RequestId is not a deduplication key; repeated RequestIds remain separate WAL commands.
Submission and cancellation results are deterministic replay outcomes, not persisted responses.
Keep final status separate from cancel results; never invent final_status=cancelled.
evidence_refs: nonempty array of objects with exactly kind and id.
kind: wal_sequence or ledger_sequence; id: positive integer.
Include submission.wal_sequence AND every cancel_attempts[].wal_sequence as WAL references.
Include every trades[].ledger_sequence as Ledger references for the total matched quantity.
No duplicate references. Each cancellation claim needs its own exact WAL reference.
References resolve only to these commands/trades in this evidence package.
Do not substitute RequestId, OrderId, or recovery.through_wal_sequence for command references.
Pre-execution book, transient events, response delivery, matching-stop reasons,
and cause claims are outside the typed response schema.

Historical evidence JSON:
{evidence_json}
"""


def run_historical_summary(
    evidence_json: str, provider: DiagnosticProvider,
) -> HistoricalSummaryRunResult:
    try:
        evidence = _load_json(evidence_json)
        validate_historical_evidence_contract(evidence)
    except (ValueError, TypeError, RecursionError) as error:
        return HistoricalSummaryRunResult(RunStatus.VALIDATION_REJECTED,
            errors=(ValidationError(ErrorCategory.SCHEMA_ERROR, "evidence", str(error)),))
    try:
        raw = provider.complete(build_historical_summary_prompt(evidence_json))
    except ProviderTimeoutError:
        return HistoricalSummaryRunResult(RunStatus.PROVIDER_TIMEOUT)
    except ProviderError:
        return HistoricalSummaryRunResult(RunStatus.PROVIDER_ERROR)
    parsed = parse_historical_summary_json(raw)
    if parsed.errors:
        return HistoricalSummaryRunResult(RunStatus.PARSE_ERROR,
            errors=parsed.errors, raw_provider_output=raw)
    assert parsed.summary is not None
    validation = validate_historical_summary(parsed.summary, evidence)
    if not validation.valid:
        return HistoricalSummaryRunResult(RunStatus.VALIDATION_REJECTED,
            errors=validation.errors, raw_provider_output=raw)
    # Free-form model prose remains only in raw_provider_output (untrusted).
    trusted = replace(parsed.summary, summary=factual_historical_summary(
        parsed.summary.facts, parsed.summary.cancel_attempts))
    return HistoricalSummaryRunResult(RunStatus.ACCEPTED,
        historical_summary=trusted, raw_provider_output=raw)
