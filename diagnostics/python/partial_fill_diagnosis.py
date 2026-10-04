"""Typed partial-fill claims and deterministic validation against D0 JSON."""

from dataclasses import dataclass
from enum import Enum
import json
from typing import Any


class Cause(str, Enum):
    INSUFFICIENT_EXECUTABLE_LIQUIDITY = "insufficient_executable_liquidity"
    UNSUPPORTED_BY_EVIDENCE = "unsupported_by_evidence"


class RemainingState(str, Enum):
    RESTING = "resting"
    FILLED = "filled"
    CANCELLED = "cancelled"


class ReferenceKind(str, Enum):
    ORDER = "order"
    LEDGER_SEQUENCE = "ledger_sequence"
    WAL_SEQUENCE = "wal_sequence"


class ErrorCategory(str, Enum):
    SCHEMA_ERROR = "schema_error"
    UNSUPPORTED_FACT = "unsupported_fact"
    CONTRADICTORY_FACT = "contradictory_fact"
    INVALID_EVIDENCE_REFERENCE = "invalid_evidence_reference"
    UNSUPPORTED_CAUSE = "unsupported_cause"


@dataclass(frozen=True)
class DiagnosisFacts:
    order_id: int
    submitted_quantity: int
    matched_quantity: int
    remaining_quantity: int
    limit_price: int
    remaining_state: RemainingState


@dataclass(frozen=True)
class EvidenceReference:
    kind: ReferenceKind
    id: int


@dataclass(frozen=True)
class PartialFillDiagnosis:
    diagnosis_type: str
    cause: Cause
    summary: str
    facts: DiagnosisFacts
    evidence_refs: tuple[EvidenceReference, ...]


@dataclass(frozen=True)
class ValidationError:
    category: ErrorCategory
    path: str
    message: str


@dataclass(frozen=True)
class ValidationResult:
    errors: tuple[ValidationError, ...] = ()

    @property
    def valid(self) -> bool:
        return not self.errors

    def to_dict(self) -> dict[str, Any]:
        return {
            "valid": self.valid,
            "errors": [
                {"category": error.category.value,
                 "path": error.path, "message": error.message}
                for error in self.errors
            ],
        }


@dataclass(frozen=True)
class DiagnosisParseResult:
    diagnosis: PartialFillDiagnosis | None
    errors: tuple[ValidationError, ...] = ()


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def _reject_constant(value: str) -> None:
    raise ValueError(f"Nonstandard JSON constant: {value}")


def _load_json(raw: str) -> Any:
    return json.loads(raw, object_pairs_hook=_unique_object,
                      parse_constant=_reject_constant)


def _object(value: Any, keys: set[str], path: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != keys:
        raise ValueError(f"{path} must be an object with exactly {sorted(keys)}")
    return value


def _integer(value: Any, path: str, minimum: int) -> int:
    # bool is an int subclass, but never a valid financial quantity or ID.
    if type(value) is not int or value < minimum:
        raise ValueError(f"{path} must be an integer >= {minimum}")
    return value


def parse_diagnosis_json(raw: str) -> DiagnosisParseResult:
    """Reject malformed/unknown fields before constructing a typed diagnosis."""
    try:
        root = _object(_load_json(raw), {
            "diagnosis_type", "cause", "summary", "facts", "evidence_refs",
        }, "diagnosis")
        if root["diagnosis_type"] != "partial_fill":
            raise ValueError("diagnosis_type must be partial_fill")
        cause = Cause(root["cause"])
        if not isinstance(root["summary"], str) or not root["summary"].strip():
            raise ValueError("summary must be a nonempty string")
        facts = _object(root["facts"], {
            "order_id", "submitted_quantity", "matched_quantity",
            "remaining_quantity", "limit_price", "remaining_state",
        }, "facts")
        typed_facts = DiagnosisFacts(
            order_id=_integer(facts["order_id"], "facts.order_id", 1),
            submitted_quantity=_integer(facts["submitted_quantity"],
                                        "facts.submitted_quantity", 1),
            matched_quantity=_integer(facts["matched_quantity"],
                                      "facts.matched_quantity", 0),
            remaining_quantity=_integer(facts["remaining_quantity"],
                                        "facts.remaining_quantity", 0),
            limit_price=_integer(facts["limit_price"], "facts.limit_price", 1),
            remaining_state=RemainingState(facts["remaining_state"]),
        )
        refs = root["evidence_refs"]
        if not isinstance(refs, list) or not refs:
            raise ValueError("evidence_refs must be a nonempty array")
        typed_refs = []
        for index, ref in enumerate(refs):
            ref = _object(ref, {"kind", "id"}, f"evidence_refs[{index}]")
            typed_refs.append(EvidenceReference(
                ReferenceKind(ref["kind"]),
                _integer(ref["id"], f"evidence_refs[{index}].id", 1),
            ))
        return DiagnosisParseResult(PartialFillDiagnosis(
            "partial_fill", cause, root["summary"], typed_facts,
            tuple(typed_refs),
        ))
    except (ValueError, TypeError) as error:
        return DiagnosisParseResult(None, (ValidationError(
            ErrorCategory.SCHEMA_ERROR, "diagnosis", str(error),
        ),))


def _lookup(evidence: dict[str, Any], path: str) -> Any:
    value: Any = evidence
    for part in path.split("."):
        if not isinstance(value, dict) or part not in value:
            return None
        value = value[part]
    return value


def _reference_ids(evidence: dict[str, Any]) -> dict[ReferenceKind, set[int]]:
    """Only explicit D0 identity fields count; never search arbitrary numbers."""
    ids: dict[ReferenceKind, set[int]] = {kind: set() for kind in ReferenceKind}

    def add(kind: ReferenceKind, item: Any, fields: tuple[str, ...]) -> None:
        if isinstance(item, dict):
            for field in fields:
                value = item.get(field)
                if type(value) is int and value > 0:
                    ids[kind].add(value)

    for section in ("target_order", "post_execution", "provenance"):
        add(ReferenceKind.ORDER, evidence.get(section), ("order_id",))
    collections = (
        _lookup(evidence, "pre_execution_book.opposing_orders"),
        _lookup(evidence, "execution.events"),
        evidence.get("trades"),
    )
    for items in collections:
        if isinstance(items, list):
            for item in items:
                add(ReferenceKind.ORDER, item,
                    ("order_id", "buy_order_id", "sell_order_id"))
    trades = evidence.get("trades")
    if isinstance(trades, list):
        for trade in trades:
            add(ReferenceKind.LEDGER_SEQUENCE, trade, ("ledger_sequence",))
    add(ReferenceKind.WAL_SEQUENCE, evidence.get("provenance"), ("wal_sequence",))
    return ids


def validate_diagnosis(
    diagnosis: PartialFillDiagnosis, evidence: dict[str, Any],
) -> ValidationResult:
    """Ground a parsed diagnosis. Summary prose is intentionally not evaluated.

    unsupported_by_evidence abstains from a cause claim; it still requires
    every factual claim and reference to be grounded. D0 does not establish
    filled/cancelled lifecycle states merely by the absence of a resting order.
    """
    if not isinstance(evidence, dict) or evidence.get("case_type") != "partial_fill":
        return ValidationResult((ValidationError(
            ErrorCategory.SCHEMA_ERROR, "evidence.case_type",
            "Expected a D0 partial_fill evidence object",
        ),))
    errors = []

    def error(category: ErrorCategory, path: str, message: str) -> None:
        errors.append(ValidationError(category, path, message))

    sources = {
        "order_id": "target_order.order_id",
        "submitted_quantity": "target_order.submitted_quantity",
        "matched_quantity": "matched_quantity",
        "remaining_quantity": "post_execution.remaining_quantity",
        "limit_price": "target_order.limit_price",
    }
    values = {}
    for fact, source in sources.items():
        actual = _lookup(evidence, source)
        values[fact] = actual
        minimum = 0 if fact in ("matched_quantity", "remaining_quantity") else 1
        if type(actual) is not int or actual < minimum:
            error(ErrorCategory.UNSUPPORTED_FACT, f"facts.{fact}",
                  f"Missing or invalid evidence.{source}")
        elif getattr(diagnosis.facts, fact) != actual:
            error(ErrorCategory.CONTRADICTORY_FACT, f"facts.{fact}",
                  f"Expected {actual} from evidence.{source}")

    resting = _lookup(evidence, "post_execution.resting")
    claimed_resting = diagnosis.facts.remaining_state == RemainingState.RESTING
    if type(resting) is not bool:
        error(ErrorCategory.UNSUPPORTED_FACT, "facts.remaining_state",
              "Missing or invalid evidence.post_execution.resting")
    elif resting != claimed_resting:
        error(ErrorCategory.CONTRADICTORY_FACT, "facts.remaining_state",
              f"Evidence post_execution.resting is {resting}")
    elif not claimed_resting:
        error(ErrorCategory.UNSUPPORTED_FACT, "facts.remaining_state",
              "D0 does not establish a filled or cancelled lifecycle state")

    if diagnosis.cause == Cause.INSUFFICIENT_EXECUTABLE_LIQUIDITY:
        submitted = values["submitted_quantity"]
        matched = values["matched_quantity"]
        remaining = values["remaining_quantity"]
        executable = _lookup(evidence, "pre_execution_book.total_executable_quantity")
        supported = (
            type(submitted) is int and submitted > 0
            and type(matched) is int and matched >= 0
            and type(remaining) is int and remaining > 0
            and type(executable) is int and executable >= 0
            and matched < submitted and executable == matched
            and submitted == matched + remaining
        )
        if not supported:
            error(ErrorCategory.UNSUPPORTED_CAUSE, "cause",
                  "Evidence does not establish exhausted executable liquidity "
                  "and quantity conservation for a partial fill")

    ids = _reference_ids(evidence)
    for index, ref in enumerate(diagnosis.evidence_refs):
        if ref.id not in ids[ref.kind]:
            error(ErrorCategory.INVALID_EVIDENCE_REFERENCE,
                  f"evidence_refs[{index}]",
                  f"No {ref.kind.value} {ref.id} in evidence")
    return ValidationResult(tuple(errors))


def validate_json_documents(evidence_json: str, diagnosis_json: str) -> ValidationResult:
    parsed = parse_diagnosis_json(diagnosis_json)
    if parsed.errors:
        return ValidationResult(parsed.errors)
    try:
        evidence = _load_json(evidence_json)
    except (ValueError, TypeError) as error:
        return ValidationResult((ValidationError(
            ErrorCategory.SCHEMA_ERROR, "evidence", str(error),
        ),))
    assert parsed.diagnosis is not None
    return validate_diagnosis(parsed.diagnosis, evidence)
