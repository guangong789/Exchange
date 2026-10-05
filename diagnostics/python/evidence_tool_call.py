"""Single-call validation for the two explicitly supported question families."""

from dataclasses import dataclass
import re
from typing import Any

from .partial_fill_diagnosis import _load_json


PARTIAL_FILL_TOOL_NAME = "get_partial_fill_evidence"
HISTORICAL_TOOL_NAME = "get_historical_order_evidence"


class ToolCallError(Exception):
    """A rejected untrusted tool request or unsupported question."""


def question_target(question: str) -> tuple[int, str]:
    # This is the existing closed question consistency guard, extended by two
    # historical forms. It does not choose/execute a tool instead of the model.
    if isinstance(question, str):
        match = re.fullmatch(
            r"Why was order ([1-9][0-9]{0,19}) (?:only )?partially filled\?",
            question.strip(), re.IGNORECASE,
        )
        if match:
            return int(match.group(1)), PARTIAL_FILL_TOOL_NAME
        match = re.fullmatch(
            r"(?:What happened to historical order ([1-9][0-9]{0,19}) after recovery"
            r"|What durable evidence do we have for order ([1-9][0-9]{0,19}))\?",
            question.strip(), re.IGNORECASE,
        )
        if match:
            return int(match.group(1) or match.group(2)), HISTORICAL_TOOL_NAME
    raise ToolCallError("Expected a supported partial-fill or historical question with one OrderId")


def question_order_id(question: str) -> int:
    return question_target(question)[0]


def tool_selection_messages(question: str) -> list[dict[str, str]]:
    return [
        {"role": "system", "content":
         "Choose exactly one tool using the OrderId in the question. For why the "
         "known execution-time partial-fill case behaved as it did, use "
         "get_partial_fill_evidence. For persisted orders, durable trade facts or "
         "final state after recovery, use get_historical_order_evidence. Historical "
         "evidence cannot prove historical pre-book, response delivery or why matching "
         "stopped. Do not diagnose or invent evidence before retrieving it."},
        {"role": "user", "content": question},
    ]


@dataclass(frozen=True)
class EvidenceToolCall:
    call_id: str
    name: str
    order_id: int
    arguments_json: str

    def to_dict(self) -> dict[str, Any]:
        return {"id": self.call_id, "name": self.name,
                "arguments": {"order_id": self.order_id}}

    def assistant_message(self) -> dict[str, Any]:
        return {"role": "assistant", "content": None, "tool_calls": [{
            "id": self.call_id, "type": "function",
            "function": {"name": self.name, "arguments": self.arguments_json},
        }]}


def validate_tool_call(
    message: Any, requested_order_id: int, expected_tool_name: str = PARTIAL_FILL_TOOL_NAME,
) -> EvidenceToolCall | None:
    if not isinstance(message, dict) or message.get("role") != "assistant":
        raise ToolCallError("Expected an assistant tool-selection message")
    calls = message.get("tool_calls")
    if calls is None or calls == []:
        return None
    if not isinstance(calls, list) or len(calls) != 1:
        raise ToolCallError("Exactly one tool call is allowed")
    call = calls[0]
    if not isinstance(call, dict) or call.get("type") != "function":
        raise ToolCallError("Expected a function tool call")
    call_id = call.get("id")
    if not isinstance(call_id, str) or not call_id.strip():
        raise ToolCallError("Tool call must have a nonempty ID")
    function = call.get("function")
    if (expected_tool_name not in (PARTIAL_FILL_TOOL_NAME, HISTORICAL_TOOL_NAME)
            or not isinstance(function, dict) or set(function) != {"name", "arguments"}
            or function["name"] != expected_tool_name):
        raise ToolCallError("Tool name must match the question's evidence scope")
    raw = function["arguments"]
    if not isinstance(raw, str):
        raise ToolCallError("Tool arguments must be a JSON string")
    try:
        arguments = _load_json(raw)
    except (ValueError, TypeError, RecursionError):
        raise ToolCallError("Tool arguments must be valid JSON") from None
    if (not isinstance(arguments, dict) or set(arguments) != {"order_id"}
            or type(arguments["order_id"]) is not int or arguments["order_id"] <= 0):
        raise ToolCallError("Tool requires exactly one positive integer order_id")
    if arguments["order_id"] != requested_order_id:
        raise ToolCallError("Tool OrderId must match the question's OrderId")
    return EvidenceToolCall(call_id, function["name"], arguments["order_id"], raw)
