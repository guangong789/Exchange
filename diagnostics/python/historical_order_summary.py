"""Narrow historical factual claims, grounded independently of diagnosis causes."""

from dataclasses import dataclass
from enum import Enum
from typing import Any

from .historical_order_evidence_source import validate_historical_evidence_contract
from .historical_replay_result import (
    CANCEL_RESULTS, INT64_MAX, OUTCOME_BASIS, ReplayResult, SUBMIT_RESULTS,
    bounded_integer,
)
from .partial_fill_diagnosis import (
    ErrorCategory, EvidenceReference, ReferenceKind, ValidationError, ValidationResult,
    _load_json, _lookup, _object,
)


class HistoricalFinalStatus(str, Enum):
    RESTING = "resting"
    FULLY_FILLED = "fully_filled"
    NOT_RESTING = "not_resting"


@dataclass(frozen=True)
class HistoricalOrderFacts:
    order_id: int
    submitted_quantity: int
    submitted_price: int
    submitted_side: str
    submission_recovered_result: ReplayResult
    matched_quantity: int
    final_status: HistoricalFinalStatus
    remaining_quantity: int | None
    outcome_basis: str
    recovered_terminal_wal_sequence: int
    torn_tail_ignored: bool


@dataclass(frozen=True)
class HistoricalCancelAttempt:
    wal_sequence: int
    account_id: int
    request_id: int
    recovered_result: ReplayResult


@dataclass(frozen=True)
class HistoricalOrderSummary:
    response_type: str
    summary: str
    facts: HistoricalOrderFacts
    cancel_attempts: tuple[HistoricalCancelAttempt, ...]
    evidence_refs: tuple[EvidenceReference, ...]


@dataclass(frozen=True)
class HistoricalSummaryParseResult:
    summary: HistoricalOrderSummary | None
    errors: tuple[ValidationError, ...] = ()


def parse_historical_summary_json(raw: str) -> HistoricalSummaryParseResult:
    try:
        root = _object(_load_json(raw), {
            "response_type", "summary", "facts", "cancel_attempts", "evidence_refs",
        }, "historical_summary")
        if root["response_type"] != "historical_order_summary":
            raise ValueError("response_type must be historical_order_summary")
        if not isinstance(root["summary"], str) or not root["summary"].strip():
            raise ValueError("summary must be a nonempty string")
        facts = _object(root["facts"], {
            "order_id", "submitted_quantity", "submitted_price", "submitted_side",
            "matched_quantity", "final_status", "remaining_quantity",
            "submission_recovered_result", "outcome_basis",
            "recovered_terminal_wal_sequence", "torn_tail_ignored",
        }, "facts")
        if facts["submitted_side"] not in ("BUY", "SELL"):
            raise ValueError("submitted_side must be BUY or SELL")
        remaining = facts["remaining_quantity"]
        if remaining is not None:
            remaining = bounded_integer(remaining, "facts.remaining_quantity", 0, INT64_MAX)
        submit_result = ReplayResult(facts["submission_recovered_result"])
        if submit_result not in SUBMIT_RESULTS:
            raise ValueError("submission_recovered_result must be a submit replay result")
        if facts["outcome_basis"] != OUTCOME_BASIS:
            raise ValueError("outcome_basis must be deterministic_replay")
        if type(facts["torn_tail_ignored"]) is not bool:
            raise ValueError("torn_tail_ignored must be a boolean")
        typed_facts = HistoricalOrderFacts(
            bounded_integer(facts["order_id"], "facts.order_id"),
            bounded_integer(facts["submitted_quantity"], "facts.submitted_quantity", maximum=INT64_MAX),
            bounded_integer(facts["submitted_price"], "facts.submitted_price", maximum=INT64_MAX),
            facts["submitted_side"],
            submit_result,
            bounded_integer(facts["matched_quantity"], "facts.matched_quantity", 0, INT64_MAX),
            HistoricalFinalStatus(facts["final_status"]), remaining,
            facts["outcome_basis"],
            bounded_integer(facts["recovered_terminal_wal_sequence"],
                            "facts.recovered_terminal_wal_sequence"),
            facts["torn_tail_ignored"],
        )
        if not isinstance(root["cancel_attempts"], list):
            raise ValueError("cancel_attempts must be an array")
        typed_attempts = []
        cancel_sequences = set()
        for index, attempt in enumerate(root["cancel_attempts"]):
            path = f"cancel_attempts[{index}]"
            attempt = _object(attempt, {
                "wal_sequence", "account_id", "request_id", "recovered_result",
            }, path)
            sequence = bounded_integer(attempt["wal_sequence"], f"{path}.wal_sequence")
            if sequence in cancel_sequences:
                raise ValueError("Duplicate cancellation WAL sequence")
            cancel_sequences.add(sequence)
            result = ReplayResult(attempt["recovered_result"])
            if result not in CANCEL_RESULTS:
                raise ValueError("Cancellation requires a cancel replay result")
            typed_attempts.append(HistoricalCancelAttempt(
                sequence, bounded_integer(attempt["account_id"], f"{path}.account_id"),
                bounded_integer(attempt["request_id"], f"{path}.request_id"), result,
            ))
        refs = root["evidence_refs"]
        if not isinstance(refs, list) or not refs:
            raise ValueError("evidence_refs must be a nonempty array")
        typed_refs = []
        ref_ids = set()
        for index, ref in enumerate(refs):
            ref = _object(ref, {"kind", "id"}, f"evidence_refs[{index}]")
            kind = ReferenceKind(ref["kind"])
            if kind not in (ReferenceKind.WAL_SEQUENCE, ReferenceKind.LEDGER_SEQUENCE):
                raise ValueError("Historical references must be wal_sequence or ledger_sequence")
            identifier = bounded_integer(ref["id"], f"evidence_refs[{index}].id")
            if (kind, identifier) in ref_ids:
                raise ValueError("Duplicate evidence reference")
            ref_ids.add((kind, identifier))
            typed_refs.append(EvidenceReference(kind, identifier))
        return HistoricalSummaryParseResult(HistoricalOrderSummary(
            root["response_type"], root["summary"], typed_facts,
            tuple(typed_attempts), tuple(typed_refs),
        ))
    except (ValueError, TypeError, RecursionError) as error:
        return HistoricalSummaryParseResult(None, (ValidationError(
            ErrorCategory.SCHEMA_ERROR, "historical_summary", str(error),
        ),))


def validate_historical_summary(
    summary: HistoricalOrderSummary, evidence: dict[str, Any],
) -> ValidationResult:
    try:
        validate_historical_evidence_contract(evidence)
    except (ValueError, TypeError, RecursionError) as error:
        return ValidationResult((ValidationError(
            ErrorCategory.SCHEMA_ERROR, "evidence", str(error),
        ),))
    errors = []
    numeric_sources = {
        "order_id": "order_id", "submitted_quantity": "submission.quantity",
        "submitted_price": "submission.price", "matched_quantity": "matched_quantity",
        "recovered_terminal_wal_sequence": "recovery.through_wal_sequence",
    }
    for fact, path in numeric_sources.items():
        actual = _lookup(evidence, path)
        minimum = 0 if fact == "matched_quantity" else 1
        if type(actual) is not int or actual < minimum:
            errors.append(ValidationError(ErrorCategory.UNSUPPORTED_FACT,
                f"facts.{fact}", f"Missing or invalid evidence.{path}"))
        elif getattr(summary.facts, fact) != actual:
            errors.append(ValidationError(ErrorCategory.CONTRADICTORY_FACT,
                f"facts.{fact}", f"Expected {actual} from evidence.{path}"))
    for fact, path, allowed in (
        ("submitted_side", "submission.side", ("BUY", "SELL")),
        ("final_status", "final_state.status", tuple(status.value for status in HistoricalFinalStatus)),
        ("submission_recovered_result", "submission.recovered_result", tuple(SUBMIT_RESULTS)),
        ("outcome_basis", "outcome_basis", (OUTCOME_BASIS,)),
    ):
        actual = _lookup(evidence, path)
        if not isinstance(actual, str) or actual not in allowed:
            errors.append(ValidationError(ErrorCategory.UNSUPPORTED_FACT,
                f"facts.{fact}", f"Missing or invalid evidence.{path}"))
        elif getattr(summary.facts, fact) != actual:
            errors.append(ValidationError(ErrorCategory.CONTRADICTORY_FACT,
                f"facts.{fact}", f"Expected {actual} from evidence.{path}"))
    state = evidence.get("final_state")
    remaining = _lookup(evidence, "final_state.remaining_quantity")
    if (not isinstance(state, dict) or "remaining_quantity" not in state
            or (remaining is not None and (type(remaining) is not int or remaining < 0))):
        errors.append(ValidationError(ErrorCategory.UNSUPPORTED_FACT,
            "facts.remaining_quantity", "Missing or invalid final remaining quantity"))
    elif summary.facts.remaining_quantity != remaining:
        errors.append(ValidationError(ErrorCategory.CONTRADICTORY_FACT,
            "facts.remaining_quantity", "Remaining quantity must match evidence, including null"))
    if summary.facts.torn_tail_ignored != evidence["recovery"]["ignored_torn_tail"]:
        errors.append(ValidationError(ErrorCategory.CONTRADICTORY_FACT,
            "facts.torn_tail_ignored", "Torn-tail claim must match the recovered prefix"))

    attempts = evidence["cancel_attempts"]
    if len(summary.cancel_attempts) != len(attempts):
        errors.append(ValidationError(ErrorCategory.CONTRADICTORY_FACT,
            "cancel_attempts", "Every cancellation attempt must be represented exactly once"))
    for index, (claim, source) in enumerate(zip(summary.cancel_attempts, attempts)):
        for field in ("wal_sequence", "account_id", "request_id", "recovered_result"):
            if getattr(claim, field) != source[field]:
                errors.append(ValidationError(ErrorCategory.CONTRADICTORY_FACT,
                    f"cancel_attempts[{index}].{field}",
                    "Cancellation claim must match its exact evidence entry in WAL order"))

    # References are scoped to this package. Every outcome needs its own WAL
    # reference; the aggregate matched quantity needs every returned trade.
    wal = _lookup(evidence, "submission.wal_sequence")
    wal_ids = {wal} if type(wal) is int and wal > 0 else set()
    wal_ids.update(attempt["wal_sequence"] for attempt in attempts)
    ledger_ids = set()
    trades = evidence.get("trades")
    if isinstance(trades, list):
        for trade in trades:
            sequence = trade.get("ledger_sequence") if isinstance(trade, dict) else None
            if type(sequence) is int and sequence > 0:
                ledger_ids.add(sequence)
    for index, ref in enumerate(summary.evidence_refs):
        ids = wal_ids if ref.kind == ReferenceKind.WAL_SEQUENCE else ledger_ids
        if ref.kind not in (ReferenceKind.WAL_SEQUENCE, ReferenceKind.LEDGER_SEQUENCE) or ref.id not in ids:
            errors.append(ValidationError(ErrorCategory.INVALID_EVIDENCE_REFERENCE,
                f"evidence_refs[{index}]", f"No {ref.kind.value} {ref.id} in historical provenance"))
    required = ({(ReferenceKind.WAL_SEQUENCE, identifier) for identifier in wal_ids}
                | {(ReferenceKind.LEDGER_SEQUENCE, identifier) for identifier in ledger_ids})
    supplied = {(ref.kind, ref.id) for ref in summary.evidence_refs}
    for kind, identifier in sorted(required - supplied, key=lambda item: (item[0].value, item[1])):
        errors.append(ValidationError(ErrorCategory.INVALID_EVIDENCE_REFERENCE,
            "evidence_refs", f"Missing {kind.value} {identifier} for a represented claim"))
    return ValidationResult(tuple(errors))


def factual_historical_summary(
    facts: HistoricalOrderFacts, cancel_attempts: tuple[HistoricalCancelAttempt, ...],
) -> str:
    """Render trusted display prose from grounded facts; never infer a cause."""
    remaining = (str(facts.remaining_quantity) if facts.remaining_quantity is not None
                 else "unavailable")
    parts = [
        f"As of the recovered WAL prefix through sequence {facts.recovered_terminal_wal_sequence} "
        f"(torn tail ignored: {'yes' if facts.torn_tail_ignored else 'no'}), "
        f"order {facts.order_id} has a durable submission of {facts.submitted_side} "
        f"{facts.submitted_quantity} @ {facts.submitted_price}, deterministically replayed as "
        f"{facts.submission_recovered_result.value}.",
        f"Recovered Ledger trades total {facts.matched_quantity}; final recovered status is "
        f"{facts.final_status.value}, with remaining quantity {remaining}.",
    ]
    for attempt in cancel_attempts:
        parts.append(
            f"Cancellation attempt at WAL sequence {attempt.wal_sequence} by account "
            f"{attempt.account_id} (RequestId {attempt.request_id}) deterministically replayed "
            f"as {attempt.recovered_result.value}.")
    parts.append("This evidence does not establish historical pre-execution book, why matching "
                 "stopped, or original response delivery.")
    return " ".join(parts)
