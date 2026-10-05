"""Read the one D0 evidence contract from an isolated, bounded C++ process."""

import os
from typing import Any

from .evidence_process import MAX_EVIDENCE_BYTES, read_evidence_process
from .partial_fill_diagnosis import _load_json


def parse_partial_fill_evidence(raw: str, order_id: int) -> dict[str, Any]:
    """Check the source contract and target identity, not D0's full invariants."""
    unavailable = {"error": "evidence_unavailable", "order_id": order_id}
    try:
        evidence = _load_json(raw)
    except (ValueError, TypeError, RecursionError):
        return unavailable
    if not isinstance(evidence, dict) or evidence.get("case_type") != "partial_fill":
        return unavailable
    for section in ("target_order", "pre_execution_book", "execution", "post_execution", "provenance"):
        if not isinstance(evidence.get(section), dict):
            return unavailable
    if (not isinstance(evidence.get("trades"), list)
            or type(evidence.get("matched_quantity")) is not int
            or evidence["matched_quantity"] < 0):
        return unavailable
    target_id = evidence["target_order"].get("order_id")
    if type(target_id) is not int or target_id <= 0:
        return unavailable
    if target_id != order_id:
        return {"error": "order_not_found", "order_id": order_id}
    return evidence


def load_partial_fill_evidence_from_executable(
    order_id: int, *, max_output_bytes: int = MAX_EVIDENCE_BYTES,
) -> dict[str, Any]:
    """Launch only the explicitly configured binary, with no shell or retries.

    The D0 executable is a Linux application. Selectable pipes bound stdout
    while enforcing a deadline; wait(timeout) also covers exit after pipe EOF.
    stderr is discarded, and the child is killed/reaped on every early failure.
    """
    if type(order_id) is not int or order_id <= 0:
        return {"error": "invalid_order_id"}

    def unavailable(reason: str) -> dict[str, Any]:
        return {"error": "evidence_unavailable", "order_id": order_id, "reason": reason}

    executable = os.environ.get("EXCHANGE_DIAGNOSTIC_EVIDENCE_BIN", "").strip()
    if not executable or "\0" in executable:
        return unavailable("executable_not_configured")
    result = read_evidence_process([executable], order_id, max_output_bytes=max_output_bytes)
    if isinstance(result, dict):
        return result
    exit_code, raw = result
    if exit_code != 0:
        return unavailable("nonzero_exit")
    return parse_partial_fill_evidence(raw, order_id)
