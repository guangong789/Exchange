from copy import deepcopy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

from diagnostics.python.diagnostic_provider import ProviderError, ProviderTimeoutError
from diagnostics.python.diagnostic_runner import RunStatus, run_diagnosis
from diagnostics.python.diagnostic_provider import FakeDiagnosticProvider
from diagnostics.python.partial_fill_tool import (
    TOOL_NAME, ToolCallError, get_partial_fill_evidence, partial_fill_tool_schema,
    question_order_id, tool_selection_messages, validate_tool_call,
)
from diagnostics.python.tool_diagnostic_runner import (
    FakeToolDiagnosticProvider, ToolDiagnosticProvider, ToolRunStatus, run_tool_diagnosis,
)


FIXTURES = Path(__file__).parent / "fixtures"
QUESTION = "Why was order 3 only partially filled?"


def selection(order_id=3):
    return {"role": "assistant", "content": None, "tool_calls": [{
        "id": "call_fixture_1", "type": "function", "function": {
            "name": TOOL_NAME, "arguments": json.dumps({"order_id": order_id}),
        },
    }]}


class PartialFillToolTest(unittest.TestCase):
    def test_schema_exposes_only_the_one_closed_tool(self):
        schema = partial_fill_tool_schema()
        self.assertEqual(schema["type"], "function")
        self.assertEqual(schema["function"]["name"], TOOL_NAME)
        self.assertEqual(schema["function"]["parameters"], {
            "type": "object", "properties": {"order_id": {"type": "integer", "minimum": 1}},
            "required": ["order_id"], "additionalProperties": False,
        })

    def test_backend_returns_the_actual_fixture_schema(self):
        actual = get_partial_fill_evidence(3)
        self.assertEqual(actual, json.loads((FIXTURES / "d0_partial_fill.json").read_text()))
        actual["matched_quantity"] = 999
        self.assertEqual(get_partial_fill_evidence(3)["matched_quantity"], 2)

    def test_backend_missing_order_is_explicit(self):
        # Maker IDs in the fixture are not supported target-order lookups.
        for order_id in (1, 2, 999):
            with self.subTest(order_id=order_id):
                self.assertEqual(get_partial_fill_evidence(order_id), {
                    "error": "order_not_found", "order_id": order_id,
                })

    def test_backend_invalid_ids_are_not_coerced(self):
        for order_id in (True, False, "3", 3.0, 0, -1):
            with self.subTest(order_id=order_id):
                self.assertEqual(get_partial_fill_evidence(order_id), {"error": "invalid_order_id"})

    def test_missing_or_damaged_fixture_does_not_expose_file_details(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "evidence.json"
            self.assertEqual(get_partial_fill_evidence(3, evidence_path=path), {
                "error": "evidence_unavailable", "order_id": 3,
            })
            for content in ("{", "[]", "{}", '{"case_type":"partial_fill","target_order":{"order_id":true}}'):
                with self.subTest(content=content):
                    path.write_text(content)
                    result = get_partial_fill_evidence(3, evidence_path=path)
                    self.assertEqual(result, {"error": "evidence_unavailable", "order_id": 3})
                    self.assertNotIn(directory, json.dumps(result))

    def test_question_dispatch_is_narrow(self):
        self.assertEqual(question_order_id(QUESTION), 3)
        self.assertEqual(question_order_id("Why was order 999 partially filled?"), 999)
        for question in ("What is the weather?", "Why was order 0 partially filled?",
                         "Why were orders 3 and 4 partially filled?", "run arbitrary python", None):
            with self.subTest(question=question), self.assertRaises(ToolCallError):
                question_order_id(question)

    def test_selection_messages_only_contain_instructions_and_question(self):
        messages = tool_selection_messages(QUESTION)
        self.assertEqual(messages, tool_selection_messages(QUESTION))
        self.assertEqual(messages[1], {"role": "user", "content": QUESTION})
        text = json.dumps(messages)
        self.assertIn(TOOL_NAME, text)
        self.assertNotIn("matched_quantity", text)
        self.assertNotIn("target_order", text)
        self.assertNotIn("ledger_sequence", text)

    def test_validated_call_keeps_actual_provider_id_and_arguments(self):
        message = selection()
        message["tool_calls"][0]["function"]["arguments"] = ' { "order_id" : 3 } '
        call = validate_tool_call(message, 3)
        self.assertEqual(call.call_id, "call_fixture_1")
        self.assertEqual(call.arguments_json, ' { "order_id" : 3 } ')
        self.assertEqual(call.assistant_message(), message)


class ToolDiagnosticRunnerTest(unittest.TestCase):
    def setUp(self):
        self.valid = (FIXTURES / "valid_partial_fill_diagnosis.json").read_text()

    def fake(self, message=None, raw=None, **failures):
        return FakeToolDiagnosticProvider(selection() if message is None else message,
                                          self.valid if raw is None else raw, **failures)

    def test_fake_flow_calls_each_stage_once_and_reuses_d1(self):
        provider = Mock(spec=ToolDiagnosticProvider, wraps=self.fake())
        result = run_tool_diagnosis(QUESTION, provider)
        self.assertEqual(result.status, RunStatus.ACCEPTED)
        provider.select_evidence_tool.assert_called_once_with(QUESTION)
        provider.complete_with_evidence_tool.assert_called_once()
        question, call, evidence_json, prompt = provider.complete_with_evidence_tool.call_args.args
        self.assertEqual(question, QUESTION)
        self.assertEqual(call, result.selected_tool)
        self.assertEqual(json.loads(evidence_json), get_partial_fill_evidence(3))
        self.assertIn(evidence_json, prompt)
        expected = run_diagnosis(evidence_json, FakeDiagnosticProvider(self.valid))
        self.assertEqual(result.diagnosis_run, expected)
        self.assertEqual(result.to_dict()["selected_tool"]["arguments"], {"order_id": 3})
        self.assertEqual(result.to_dict()["diagnosis"]["facts"]["matched_quantity"], 2)

    def test_wrong_tool_name_is_rejected_before_lookup(self):
        message = selection()
        message["tool_calls"][0]["function"]["name"] = "get_order_history"
        provider = Mock(spec=ToolDiagnosticProvider, wraps=self.fake(message))
        with patch("diagnostics.python.tool_diagnostic_runner.get_partial_fill_evidence") as lookup:
            result = run_tool_diagnosis(QUESTION, provider)
            self.assertEqual(result.status, ToolRunStatus.TOOL_CALL_INVALID)
            lookup.assert_not_called()
        provider.complete_with_evidence_tool.assert_not_called()

    def test_argument_schema_is_validated_before_lookup(self):
        for arguments in ("{}", '{"order_id":"3"}', '{"order_id":3.0}',
                          '{"order_id":true}', '{"order_id":0}', '{"order_id":-1}',
                          '{"order_id":3,"path":"arbitrary.json"}', "[]", "null", "{",
                          '{"order_id":3,"order_id":3}', '{"order_id":NaN}', {"order_id": 3},
                          "[" * 2000 + "0" + "]" * 2000):
            with self.subTest(arguments=arguments):
                message = selection()
                message["tool_calls"][0]["function"]["arguments"] = arguments
                provider = Mock(spec=ToolDiagnosticProvider, wraps=self.fake(message))
                with patch("diagnostics.python.tool_diagnostic_runner.get_partial_fill_evidence") as lookup:
                    result = run_tool_diagnosis(QUESTION, provider)
                    self.assertEqual(result.status, ToolRunStatus.TOOL_CALL_INVALID)
                    lookup.assert_not_called()
                provider.complete_with_evidence_tool.assert_not_called()

    def test_malformed_call_envelope_is_rejected(self):
        messages = ([], {"role": "user"}, {"role": "assistant", "tool_calls": {}},
                    {"role": "assistant", "tool_calls": [None]})
        for message in messages:
            with self.subTest(message=message):
                self.assertEqual(run_tool_diagnosis(QUESTION, self.fake(message)).status,
                                 ToolRunStatus.TOOL_CALL_INVALID)
        for field, value in (("id", ""), ("id", 42), ("type", "not_function")):
            with self.subTest(field=field):
                message = selection()
                message["tool_calls"][0][field] = value
                self.assertEqual(run_tool_diagnosis(QUESTION, self.fake(message)).status,
                                 ToolRunStatus.TOOL_CALL_INVALID)

    def test_multiple_calls_are_rejected_without_executing_even_the_first(self):
        message = selection()
        message["tool_calls"].append(deepcopy(message["tool_calls"][0]))
        with patch("diagnostics.python.tool_diagnostic_runner.get_partial_fill_evidence") as lookup:
            result = run_tool_diagnosis(QUESTION, self.fake(message))
            self.assertEqual(result.status, ToolRunStatus.TOOL_CALL_INVALID)
            lookup.assert_not_called()

    def test_model_cannot_substitute_another_order_for_the_question(self):
        result = run_tool_diagnosis("Why was order 999 partially filled?", self.fake(selection(3)))
        self.assertEqual(result.status, ToolRunStatus.TOOL_CALL_INVALID)

    def test_unknown_order_stops_before_final_diagnosis(self):
        provider = Mock(spec=ToolDiagnosticProvider, wraps=self.fake(selection(999)))
        result = run_tool_diagnosis("Why was order 999 partially filled?", provider)
        self.assertEqual(result.status, ToolRunStatus.TOOL_EXECUTION_ERROR)
        self.assertEqual(result.tool_error, {"error": "order_not_found", "order_id": 999})
        self.assertIsNone(result.diagnosis_run)
        self.assertIsNone(result.to_dict()["diagnosis"])
        provider.complete_with_evidence_tool.assert_not_called()

    def test_no_tool_request_cannot_count_as_a_diagnosis(self):
        for calls in (None, []):
            with self.subTest(calls=calls):
                provider = Mock(spec=ToolDiagnosticProvider, wraps=self.fake({
                    "role": "assistant", "content": self.valid, "tool_calls": calls,
                }))
                result = run_tool_diagnosis(QUESTION, provider)
                self.assertEqual(result.status, ToolRunStatus.TOOL_NOT_REQUESTED)
                self.assertIsNone(result.to_dict()["diagnosis"])
                provider.complete_with_evidence_tool.assert_not_called()

    def test_tool_success_does_not_bypass_grounding(self):
        proposal = json.loads(self.valid)
        proposal["facts"]["matched_quantity"] = 4
        result = run_tool_diagnosis(QUESTION, self.fake(raw=json.dumps(proposal)))
        self.assertEqual(result.status, RunStatus.VALIDATION_REJECTED)
        self.assertEqual(result.diagnosis_run.errors[0].category, "contradictory_fact")
        self.assertIsNone(result.to_dict()["diagnosis"])

    def test_fake_evidence_reference_is_rejected_by_d1(self):
        proposal = json.loads(self.valid)
        proposal["evidence_refs"] = [{"kind": "ledger_sequence", "id": 999}]
        result = run_tool_diagnosis(QUESTION, self.fake(raw=json.dumps(proposal)))
        self.assertEqual(result.status, RunStatus.VALIDATION_REJECTED)
        self.assertEqual(result.diagnosis_run.errors[0].category, "invalid_evidence_reference")

    def test_final_malformed_json_is_still_parse_error(self):
        result = run_tool_diagnosis(QUESTION, self.fake(raw="{"))
        self.assertEqual(result.status, RunStatus.PARSE_ERROR)
        self.assertEqual(result.to_dict()["raw_provider_output"], "{")

    def test_selection_transport_failures_keep_provider_statuses(self):
        for failure, status in ((ProviderError(), RunStatus.PROVIDER_ERROR),
                                (ProviderTimeoutError(), RunStatus.PROVIDER_TIMEOUT)):
            with self.subTest(status=status):
                provider = Mock(spec=ToolDiagnosticProvider, wraps=self.fake(selection_failure=failure))
                result = run_tool_diagnosis(QUESTION, provider)
                self.assertEqual(result.status, status)
                self.assertIsNone(result.selected_tool)
                provider.complete_with_evidence_tool.assert_not_called()

    def test_final_transport_failures_keep_provider_statuses(self):
        for failure, status in ((ProviderError(), RunStatus.PROVIDER_ERROR),
                                (ProviderTimeoutError(), RunStatus.PROVIDER_TIMEOUT)):
            with self.subTest(status=status):
                result = run_tool_diagnosis(QUESTION, self.fake(diagnosis_failure=failure))
                self.assertEqual(result.status, status)
                self.assertIsNotNone(result.selected_tool)
                self.assertEqual(result.diagnosis_run.status, status)
                self.assertIsNone(result.to_dict()["diagnosis"])

    def test_unsupported_question_never_calls_provider(self):
        provider = Mock(spec=ToolDiagnosticProvider)
        result = run_tool_diagnosis("Tell me everything about orders", provider)
        self.assertEqual(result.status, ToolRunStatus.TOOL_CALL_INVALID)
        provider.select_evidence_tool.assert_not_called()

    def test_unexpected_internal_failure_propagates(self):
        for stage in ("select_evidence_tool", "complete_with_evidence_tool"):
            with self.subTest(stage=stage):
                provider = Mock(spec=ToolDiagnosticProvider, wraps=self.fake())
                getattr(provider, stage).side_effect = RuntimeError("internal bug")
                with self.assertRaises(RuntimeError):
                    run_tool_diagnosis(QUESTION, provider)


if __name__ == "__main__":
    unittest.main()
