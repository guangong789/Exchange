"""The execution-time partial-fill tool, backed by a D0 fixture or isolated executable."""

from pathlib import Path
from typing import Any

from .evidence_tool_call import (
    ToolCallError, question_order_id, tool_selection_messages, validate_tool_call,
)
from .partial_fill_evidence_source import (
    load_partial_fill_evidence_from_executable, parse_partial_fill_evidence,
)


TOOL_NAME = "get_partial_fill_evidence"
DEFAULT_EVIDENCE_PATH = Path(__file__).parent / "tests/fixtures/d0_partial_fill.json"


def partial_fill_tool_schema() -> dict[str, Any]:
    return {
        "type": "function",
        "function": {
            "name": TOOL_NAME,
            "description": "Retrieve rich execution-time evidence for why the known deterministic partial-fill scenario behaved as it did, including opposing pre-book, matching events, and trades. Not historical order recovery.",
            "parameters": {
                "type": "object",
                "properties": {"order_id": {"type": "integer", "minimum": 1}},
                "required": ["order_id"],
                "additionalProperties": False,
            },
        },
    }


def get_partial_fill_evidence(
    order_id: int, *, evidence_path: Path = DEFAULT_EVIDENCE_PATH,
    evidence_source: str = "fixture",
) -> dict[str, Any]:
    if type(order_id) is not int or order_id <= 0:
        return {"error": "invalid_order_id"}
    if evidence_source == "executable":
        return load_partial_fill_evidence_from_executable(order_id)
    if evidence_source != "fixture":
        return {"error": "evidence_unavailable", "order_id": order_id, "reason": "invalid_source"}
    try:
        raw = evidence_path.read_text(encoding="utf-8")
    except (OSError, UnicodeError):
        return {"error": "evidence_unavailable", "order_id": order_id}
    # A fresh JSON object on every call; callers cannot alter the backing fixture.
    return parse_partial_fill_evidence(raw, order_id)
