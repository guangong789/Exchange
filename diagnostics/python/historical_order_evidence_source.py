"""Validate historical lifecycle evidence from an explicitly configured C++ CLI."""

import os
from typing import Any

from .evidence_process import MAX_EVIDENCE_BYTES, read_evidence_process
from .historical_replay_result import (
    CANCEL_RESULTS, INT64_MAX, OUTCOME_BASIS, ReplayResult, SUBMIT_RESULTS,
    UINT32_MAX, UINT64_MAX, bounded_integer,
)
from .partial_fill_diagnosis import _load_json, _object


def validate_historical_evidence_contract(evidence: Any) -> None:
    """Validate the boundary contract; production C++ owns replay invariants."""
    root = _object(evidence, {
        "case_type", "order_id", "outcome_basis", "submission", "cancel_attempts",
        "trades", "matched_quantity", "final_state", "recovery", "evidence_scope",
        "unavailable_context",
    }, "evidence")
    if root["case_type"] != "historical_durable_order" or root["outcome_basis"] != OUTCOME_BASIS:
        raise ValueError("Expected historical_durable_order with deterministic_replay outcomes")
    order_id = bounded_integer(root["order_id"], "order_id")
    if root["evidence_scope"] != [
            "durable_submission", "recovered_ledger", "recovered_final_state",
            "recovered_trading_outcomes"]:
        raise ValueError("Unexpected historical evidence scope")
    unavailable = _object(root["unavailable_context"], {
        "pre_execution_book", "execution_events", "response_delivery", "matching_stop_reason",
    }, "unavailable_context")
    if any(value is not None for value in unavailable.values()):
        raise ValueError("Unavailable historical context must remain null")
    recovery = _object(root["recovery"], {
        "through_wal_sequence", "ignored_torn_tail",
    }, "recovery")
    terminal = bounded_integer(recovery["through_wal_sequence"], "recovery.through_wal_sequence")
    if type(recovery["ignored_torn_tail"]) is not bool:
        raise ValueError("recovery.ignored_torn_tail must be a boolean")
    submission = _object(root["submission"], {
        "request_id", "account_id", "side", "price", "quantity", "logical_timestamp",
        "wal_sequence", "recovered_result",
    }, "submission")
    for field in ("request_id", "account_id", "wal_sequence"):
        bounded_integer(submission[field], f"submission.{field}")
    for field in ("price", "quantity", "logical_timestamp"):
        bounded_integer(submission[field], f"submission.{field}", maximum=INT64_MAX)
    if submission["side"] not in ("BUY", "SELL"):
        raise ValueError("Invalid submission side")
    if ReplayResult(submission["recovered_result"]) not in SUBMIT_RESULTS:
        raise ValueError("Replay result is incompatible with submission")
    if submission["wal_sequence"] > terminal:
        raise ValueError("Submission lies beyond the recovered WAL prefix")
    bounded_integer(root["matched_quantity"], "matched_quantity", 0, INT64_MAX)
    if not isinstance(root["cancel_attempts"], list):
        raise ValueError("cancel_attempts must be an array")
    previous = 0
    for index, attempt in enumerate(root["cancel_attempts"]):
        path = f"cancel_attempts[{index}]"
        attempt = _object(attempt, {
            "request_id", "account_id", "order_id", "wal_sequence", "recovered_result",
        }, path)
        for field in ("request_id", "account_id", "order_id", "wal_sequence"):
            bounded_integer(attempt[field], f"{path}.{field}")
        sequence = attempt["wal_sequence"]
        if (attempt["order_id"] != order_id or not previous < sequence <= terminal
                or sequence == submission["wal_sequence"]):
            raise ValueError("Cancel identity/order does not match this recovered prefix")
        if ReplayResult(attempt["recovered_result"]) not in CANCEL_RESULTS:
            raise ValueError("Replay result is incompatible with cancellation")
        previous = sequence
    if not isinstance(root["trades"], list):
        raise ValueError("trades must be an array")
    previous = 0
    for index, trade in enumerate(root["trades"]):
        path = f"trades[{index}]"
        trade = _object(trade, {
            "buy_order_id", "sell_order_id", "price", "quantity",
            "logical_timestamp", "ledger_sequence",
        }, path)
        for field in ("buy_order_id", "sell_order_id", "ledger_sequence"):
            bounded_integer(trade[field], f"{path}.{field}")
        for field in ("price", "quantity", "logical_timestamp"):
            bounded_integer(trade[field], f"{path}.{field}", maximum=INT64_MAX)
        target_field = "buy_order_id" if submission["side"] == "BUY" else "sell_order_id"
        if trade[target_field] != order_id or trade["ledger_sequence"] <= previous:
            raise ValueError("Trade identity/order does not match this historical order")
        previous = trade["ledger_sequence"]
    state = _object(root["final_state"], {
        "status", "remaining_quantity", "side", "price", "reservation",
    }, "final_state")
    if state["status"] == "resting":
        bounded_integer(state["remaining_quantity"], "final_state.remaining_quantity", maximum=INT64_MAX)
        if state["side"] != submission["side"] or state["price"] != submission["price"]:
            raise ValueError("Resting order identity differs from submission")
        bounded_integer(state["price"], "final_state.price", maximum=INT64_MAX)
        reservation = _object(state["reservation"], {
            "account_id", "asset_id", "original_amount", "remaining_amount",
        }, "final_state.reservation")
        bounded_integer(reservation["account_id"], "reservation.account_id")
        bounded_integer(reservation["asset_id"], "reservation.asset_id", maximum=UINT32_MAX)
        for field in ("original_amount", "remaining_amount"):
            bounded_integer(reservation[field], f"reservation.{field}", maximum=INT64_MAX)
        if reservation["account_id"] != submission["account_id"]:
            raise ValueError("Reservation owner differs from submission")
    elif state["status"] in ("fully_filled", "not_resting"):
        if any(state[field] is not None for field in ("side", "price", "reservation")):
            raise ValueError("Non-resting order fields must be unavailable")
        remaining = state["remaining_quantity"]
        if state["status"] == "fully_filled":
            if type(remaining) is not int or remaining != 0:
                raise ValueError("Fully filled remaining quantity must be zero")
        elif remaining is not None:
            raise ValueError("not_resting remaining quantity must be unavailable")
    else:
        raise ValueError("Unknown final recovered state")


def parse_historical_order_evidence(raw: str, order_id: int) -> dict[str, Any]:
    """Check scope, structure, and identity; C++ owns recovery invariants."""
    unavailable = {"error": "evidence_unavailable", "order_id": order_id}
    try:
        evidence = _load_json(raw)
        # Preserve the lookup error for a producer returning a different ID.
        target = evidence.get("order_id") if isinstance(evidence, dict) else None
        if (isinstance(evidence, dict) and evidence.get("case_type") == "historical_durable_order"
                and type(target) is int and 0 < target <= UINT64_MAX and target != order_id):
            return {"error": "order_not_found", "order_id": order_id}
        validate_historical_evidence_contract(evidence)
    except (ValueError, TypeError, RecursionError):
        return unavailable
    return evidence


def load_historical_order_evidence_from_executable(
    order_id: int, *, max_output_bytes: int = MAX_EVIDENCE_BYTES,
) -> dict[str, Any]:
    if type(order_id) is not int or order_id <= 0:
        return {"error": "invalid_order_id"}
    settings = [os.environ.get(name, "").strip() for name in (
        "EXCHANGE_HISTORICAL_EVIDENCE_BIN", "EXCHANGE_HISTORICAL_EVIDENCE_WAL",
        "EXCHANGE_HISTORICAL_EVIDENCE_CONFIG",
    )]
    if any(not value or "\0" in value for value in settings):
        return {"error": "evidence_unavailable", "order_id": order_id,
                "reason": "historical_source_not_configured"}
    executable, wal_path, config_path = settings
    result = read_evidence_process([
        executable, "--wal", wal_path, "--order-id", str(order_id), "--config", config_path,
    ], order_id, max_output_bytes=max_output_bytes)
    if isinstance(result, dict):
        return result
    exit_code, raw = result
    if exit_code == 2:
        # D2-C defines this precise exit-code/JSON pair as order_not_found.
        try:
            error = _load_json(raw)
        except (ValueError, TypeError, RecursionError):
            error = None
        if (isinstance(error, dict) and set(error) == {"error", "order_id"}
                and error["error"] == "order_not_found"
                and type(error["order_id"]) is int and error["order_id"] == order_id):
            return error
    if exit_code != 0:
        return {"error": "evidence_unavailable", "order_id": order_id, "reason": "nonzero_exit"}
    return parse_historical_order_evidence(raw, order_id)
