from contextlib import redirect_stderr, redirect_stdout
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import MagicMock, Mock, patch
from urllib.error import HTTPError

from diagnostics.python.deepseek_diagnostic_provider import (
    MAX_RESPONSE_BYTES, DeepSeekDiagnosticProvider,
)
from diagnostics.python.diagnostic_runner import RunStatus
from diagnostics.python.historical_order_tool import get_historical_order_evidence
from diagnostics.python.partial_fill_tool import TOOL_NAME, get_partial_fill_evidence
from diagnostics.python.run_tool_diagnosis import main
from diagnostics.python.tool_diagnostic_runner import ToolRunStatus, run_tool_diagnosis


FIXTURES = Path(__file__).parent / "fixtures"
QUESTION = "Why was order 3 only partially filled?"


class DeepSeekToolDiagnosticProviderTest(unittest.TestCase):
    def setUp(self):
        environment = patch.dict(os.environ, {
            "DEEPSEEK_API_KEY": "test-placeholder-not-a-real-api-key",
        }, clear=True)
        environment.start()
        self.addCleanup(environment.stop)
        transport = patch("diagnostics.python.deepseek_diagnostic_provider.build_opener")
        self.build_opener = transport.start()
        self.addCleanup(transport.stop)
        self.opener = Mock()
        self.build_opener.return_value = self.opener
        self.valid = (FIXTURES / "valid_partial_fill_diagnosis.json").read_text()
        self.selection = {"role": "assistant", "content": None, "tool_calls": [{
            "id": "actual_provider_call_42", "type": "function", "function": {
                "name": TOOL_NAME, "arguments": '{"order_id":3}',
            },
        }]}
        self.final = {"role": "assistant", "content": self.valid}
        self.queue(self.selection, self.final)

    def response(self, message):
        response = MagicMock()
        response.status = 200
        response.__enter__.return_value = response
        response.read.side_effect = io.BytesIO(json.dumps({
            "choices": [{"message": message}],
        }).encode()).read
        return response

    def queue(self, *messages):
        self.responses = [self.response(message) for message in messages]
        self.opener.open.side_effect = self.responses

    def run_live_boundary(self, question=QUESTION):
        return run_tool_diagnosis(question, DeepSeekDiagnosticProvider())

    def test_two_requests_use_real_tool_call_and_trusted_result(self):
        result = self.run_live_boundary()
        self.assertEqual(result.status, RunStatus.ACCEPTED)
        self.assertEqual(self.opener.open.call_count, 2)
        first, second = [json.loads(call.args[0].data) for call in self.opener.open.call_args_list]
        self.assertEqual([tool["function"]["name"] for tool in first["tools"]],
                         [TOOL_NAME, "get_historical_order_evidence"])
        self.assertEqual(first["tools"][0]["function"]["name"], TOOL_NAME)
        self.assertEqual(first["tool_choice"], "auto")
        self.assertNotIn("response_format", first)
        self.assertEqual(first["messages"][1], {"role": "user", "content": QUESTION})
        self.assertNotIn("target_order", json.dumps(first))
        self.assertNotIn("matched_quantity", json.dumps(first))
        self.assertNotIn("tools", second)
        self.assertEqual(second["response_format"], {"type": "json_object"})
        self.assertEqual(second["messages"][2], self.selection)
        tool_message = second["messages"][3]
        self.assertEqual(tool_message["role"], "tool")
        self.assertEqual(tool_message["tool_call_id"], "actual_provider_call_42")
        self.assertEqual(json.loads(tool_message["content"]), get_partial_fill_evidence(3))
        self.assertIn(tool_message["content"], second["messages"][4]["content"])
        self.assertEqual(result.selected_tool.order_id, 3)
        for response in self.responses:
            response.read.assert_called_once_with(MAX_RESPONSE_BYTES + 1)
        for call in self.opener.open.call_args_list:
            self.assertEqual(call.kwargs, {"timeout": 10.0})

    def test_declined_tool_request_stops_after_one_request(self):
        self.queue({"role": "assistant", "content": "I decline to retrieve evidence."})
        result = self.run_live_boundary()
        self.assertEqual(result.status, ToolRunStatus.TOOL_NOT_REQUESTED)
        self.opener.open.assert_called_once()

    def test_invalid_real_tool_request_stops_before_execution(self):
        self.selection["tool_calls"][0]["function"]["name"] = "get_order_history"
        self.queue(self.selection)
        with patch("diagnostics.python.tool_diagnostic_runner.get_partial_fill_evidence") as lookup:
            result = self.run_live_boundary()
            self.assertEqual(result.status, ToolRunStatus.TOOL_CALL_INVALID)
            lookup.assert_not_called()
        self.opener.open.assert_called_once()

    def test_missing_evidence_stops_after_one_request(self):
        self.selection["tool_calls"][0]["function"]["arguments"] = '{"order_id":999}'
        self.queue(self.selection)
        result = self.run_live_boundary("Why was order 999 partially filled?")
        self.assertEqual(result.status, ToolRunStatus.TOOL_EXECUTION_ERROR)
        self.assertEqual(result.tool_error, {"error": "order_not_found", "order_id": 999})
        self.opener.open.assert_called_once()

    def test_final_tool_request_is_not_executed_or_retried(self):
        self.final["tool_calls"] = self.selection["tool_calls"]
        self.queue(self.selection, self.final)
        with patch("diagnostics.python.tool_diagnostic_runner.get_partial_fill_evidence",
                   wraps=get_partial_fill_evidence) as lookup:
            result = self.run_live_boundary()
            self.assertEqual(result.status, RunStatus.PROVIDER_ERROR)
            lookup.assert_called_once()
        self.assertEqual(self.opener.open.call_count, 2)

    def test_final_raw_text_is_not_repaired(self):
        self.final["content"] = "```json\n" + self.valid + "\n```"
        self.queue(self.selection, self.final)
        result = self.run_live_boundary()
        self.assertEqual(result.status, RunStatus.PARSE_ERROR)
        self.assertEqual(result.to_dict()["raw_provider_output"], self.final["content"])

    def test_final_contradiction_still_uses_existing_validator(self):
        proposal = json.loads(self.valid)
        proposal["facts"]["matched_quantity"] = 4
        self.final["content"] = json.dumps(proposal)
        self.queue(self.selection, self.final)
        result = self.run_live_boundary()
        self.assertEqual(result.status, RunStatus.VALIDATION_REJECTED)
        self.assertEqual(result.to_dict()["errors"][0]["category"], "contradictory_fact")

    def test_historical_selection_uses_two_tools_then_only_summary_schema(self):
        question = "What happened to historical order 2 after recovery?"
        self.selection["tool_calls"][0]["function"] = {
            "name": "get_historical_order_evidence", "arguments": '{"order_id":2}',
        }
        self.final["content"] = (FIXTURES / "valid_historical_order_summary.json").read_text()
        self.queue(self.selection, self.final)
        result = self.run_live_boundary(question)
        self.assertEqual(result.status, RunStatus.ACCEPTED)
        self.assertEqual(self.opener.open.call_count, 2)
        first, second = [json.loads(call.args[0].data) for call in self.opener.open.call_args_list]
        self.assertEqual([tool["function"]["name"] for tool in first["tools"]],
                         ["get_partial_fill_evidence", "get_historical_order_evidence"])
        self.assertNotIn("tools", second)
        self.assertEqual(second["messages"][2], self.selection)
        evidence = json.loads(second["messages"][3]["content"])
        self.assertEqual(evidence["case_type"], "historical_durable_order")
        self.assertEqual(second["messages"][3]["tool_call_id"], "actual_provider_call_42")
        self.assertIn("historical_order_summary", second["messages"][4]["content"])
        self.assertNotIn("insufficient_executable_liquidity", second["messages"][4]["content"])

    def test_historical_second_stage_cannot_request_another_tool(self):
        self.selection["tool_calls"][0]["function"] = {
            "name": "get_historical_order_evidence", "arguments": '{"order_id":2}',
        }
        self.final["tool_calls"] = self.selection["tool_calls"]
        self.queue(self.selection, self.final)
        with patch("diagnostics.python.tool_diagnostic_runner.get_historical_order_evidence",
                   wraps=get_historical_order_evidence) as lookup:
            result = self.run_live_boundary("What happened to historical order 2 after recovery?")
        self.assertEqual(result.status, RunStatus.PROVIDER_ERROR)
        self.assertEqual(self.opener.open.call_count, 2)
        lookup.assert_called_once()

    def test_http_failure_in_each_stage_keeps_provider_error_status(self):
        for stage in (1, 2):
            with self.subTest(stage=stage):
                self.opener.open.reset_mock()
                failure = HTTPError("https://example.invalid", 500, "failure", {}, io.BytesIO())
                self.opener.open.side_effect = ([failure] if stage == 1 else [self.response(self.selection), failure])
                result = self.run_live_boundary()
                self.assertEqual(result.status, RunStatus.PROVIDER_ERROR)
                self.assertEqual(self.opener.open.call_count, stage)

    def test_timeout_in_each_stage_keeps_provider_timeout_status(self):
        for stage in (1, 2):
            with self.subTest(stage=stage):
                self.opener.open.reset_mock()
                failure = TimeoutError("timeout")
                self.opener.open.side_effect = ([failure] if stage == 1 else [self.response(self.selection), failure])
                result = self.run_live_boundary()
                self.assertEqual(result.status, RunStatus.PROVIDER_TIMEOUT)
                self.assertEqual(self.opener.open.call_count, stage)

    def test_oversized_selection_is_provider_error(self):
        self.responses[0].read.side_effect = io.BytesIO(b"x" * (MAX_RESPONSE_BYTES + 100)).read
        self.assertEqual(self.run_live_boundary().status, RunStatus.PROVIDER_ERROR)
        self.responses[0].read.assert_called_once_with(MAX_RESPONSE_BYTES + 1)
        self.opener.open.assert_called_once()

    def run_cli(self, *arguments):
        output = io.StringIO()
        with patch("sys.argv", ["run_tool_diagnosis", QUESTION,
                                "--evidence-source", "fixture", *arguments]), redirect_stdout(output):
            code = main()
        return code, json.loads(output.getvalue())

    def test_explicit_deepseek_cli_uses_mocked_transport(self):
        code, output = self.run_cli("--provider", "deepseek")
        self.assertEqual(code, 0)
        self.assertEqual(output["status"], "accepted")
        self.assertEqual(output["selected_tool"]["name"], TOOL_NAME)
        self.assertEqual(self.opener.open.call_count, 2)

    def test_fake_cli_does_not_access_network(self):
        with tempfile.TemporaryDirectory() as directory:
            selection_file = Path(directory) / "selection.json"
            response_file = Path(directory) / "response.json"
            selection_file.write_text(json.dumps(self.selection))
            response_file.write_text(self.valid)
            code, output = self.run_cli("--selection-file", str(selection_file),
                                        "--response-file", str(response_file))
        self.assertEqual(code, 0)
        self.assertEqual(output["status"], "accepted")
        self.build_opener.assert_not_called()

    def test_cli_is_opt_in_and_does_not_mix_fake_and_real_modes(self):
        for arguments in ([], ["--provider", "deepseek", "--response-file", "anything"]):
            with self.subTest(arguments=arguments), redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as raised:
                    self.run_cli(*arguments)
                self.assertEqual(raised.exception.code, 2)
        self.build_opener.assert_not_called()


if __name__ == "__main__":
    unittest.main()
