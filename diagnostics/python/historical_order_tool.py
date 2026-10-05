"""The explicit historical tool, with offline fixtures available for tests."""

from pathlib import Path
from typing import Any

from .historical_order_evidence_source import (
    load_historical_order_evidence_from_executable, parse_historical_order_evidence,
)


TOOL_NAME = "get_historical_order_evidence"
DEFAULT_HISTORICAL_EVIDENCE_PATH = Path(__file__).parent / "tests/fixtures/d2c_historical_resting.json"


def historical_order_tool_schema() -> dict[str, Any]:
    return {
        "type": "function",
        "function": {
            "name": TOOL_NAME,
            "description": (
                "Retrieve durable submission, recovered Ledger trades, and final recovered "
                "state for a persisted order from an offline WAL and bootstrap. Use for "
                "historical/recovery facts. Does not provide historical pre-book, transient "
                "events, response delivery, or the cause matching stopped."
            ),
            "parameters": {
                "type": "object",
                "properties": {"order_id": {"type": "integer", "minimum": 1}},
                "required": ["order_id"], "additionalProperties": False,
            },
        },
    }


def get_historical_order_evidence(
    order_id: int, *, evidence_source: str = "executable",
    evidence_path: Path = DEFAULT_HISTORICAL_EVIDENCE_PATH,
) -> dict[str, Any]:
    if type(order_id) is not int or order_id <= 0:
        return {"error": "invalid_order_id"}
    if evidence_source == "executable":
        return load_historical_order_evidence_from_executable(order_id)
    if evidence_source != "fixture":
        return {"error": "evidence_unavailable", "order_id": order_id, "reason": "invalid_source"}
    try:
        raw = evidence_path.read_text(encoding="utf-8")
    except (OSError, UnicodeError):
        return {"error": "evidence_unavailable", "order_id": order_id}
    return parse_historical_order_evidence(raw, order_id)
