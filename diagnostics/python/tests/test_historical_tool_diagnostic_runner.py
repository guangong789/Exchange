from contextlib import redirect_stdout
from copy import deepcopy
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

from diagnostics.python.diagnostic_runner import RunStatus
from diagnostics.python.evidence_tool_call import HISTORICAL_TOOL_NAME, PARTIAL_FILL_TOOL_NAME
from diagnostics.python.historical_order_tool import get_historical_order_evidence, historical_order_tool_schema
from diagnostics.python.run_tool_diagnosis import main
from diagnostics.python.tool_diagnostic_runner import (
    FakeToolDiagnosticProvider, ToolDiagnosticProvider, ToolRunStatus, run_tool_diagnosis,
)


FIXTURES = Path(__file__).parent / "fixtures"
QUESTION = "What happened to historical order 2 after recovery?"


def selection(order_id=2, name=HISTORICAL_TOOL_NAME):
    return {"role": "assistant", "content": None, "tool_calls": [{
        "id": "actual_historical_call", "type": "function", "function": {
            "name": name, "arguments": json.dumps({"order_id": order_id}),
        },
    }]}


class HistoricalToolRunnerTest(unittest.TestCase):
    def setUp(self):
        self.valid = (FIXTURES / "valid_historical_order_summary.json").read_text()

    def test_historical_selection_retrieval_and_summary_each_run_once(self):
        provider = Mock(spec=ToolDiagnosticProvider, wraps=FakeToolDiagnosticProvider(selection(), self.valid))
        result = run_tool_diagnosis(QUESTION, provider)
        self.assertEqual(result.status, RunStatus.ACCEPTED)
        self.assertIsNone(result.diagnosis_run)
        self.assertEqual(result.historical_run.historical_summary.facts.order_id, 2)
        provider.select_evidence_tool.assert_called_once_with(QUESTION)
        provider.complete_with_evidence_tool.assert_called_once()
        question, call, evidence_json, prompt = provider.complete_with_evidence_tool.call_args.args
        self.assertEqual(call.name, HISTORICAL_TOOL_NAME)
        self.assertEqual(json.loads(evidence_json)["case_type"], "historical_durable_order")
        self.assertIn(evidence_json, prompt)
        self.assertIn("historical_order_summary", prompt)
        self.assertNotIn("diagnosis", result.to_dict())

    def test_second_historical_question_form_preserves_order_identity(self):
        result = run_tool_diagnosis("What durable evidence do we have for order 2?",
                                    FakeToolDiagnosticProvider(selection(), self.valid))
        self.assertEqual(result.status, RunStatus.ACCEPTED)

    def test_both_wrong_tool_selections_stop_before_any_execution(self):
        for question, message in ((QUESTION, selection(name=PARTIAL_FILL_TOOL_NAME)),
                                  ("Why was order 3 only partially filled?", selection(3))):
            with self.subTest(question=question):
                provider = Mock(spec=ToolDiagnosticProvider, wraps=FakeToolDiagnosticProvider(message, self.valid))
                with patch("diagnostics.python.tool_diagnostic_runner.get_partial_fill_evidence") as partial, \
                        patch("diagnostics.python.tool_diagnostic_runner.get_historical_order_evidence") as historical:
                    result = run_tool_diagnosis(question, provider)
                    self.assertEqual(result.status, ToolRunStatus.TOOL_CALL_INVALID)
                    partial.assert_not_called()
                    historical.assert_not_called()
                provider.complete_with_evidence_tool.assert_not_called()

    def test_missing_order_and_source_failures_stop_before_summary(self):
        provider = Mock(spec=ToolDiagnosticProvider, wraps=FakeToolDiagnosticProvider(selection(999), self.valid))
        result = run_tool_diagnosis("What durable evidence do we have for order 999?", provider)
        self.assertEqual(result.status, ToolRunStatus.TOOL_EXECUTION_ERROR)
        self.assertEqual(result.tool_error, {"error": "order_not_found", "order_id": 999})
        self.assertIsNone(result.to_dict()["historical_summary"])
        self.assertNotIn("diagnosis", result.to_dict())
        provider.complete_with_evidence_tool.assert_not_called()
        with patch("diagnostics.python.tool_diagnostic_runner.get_historical_order_evidence",
                   return_value={"error": "evidence_unavailable", "order_id": 2}):
            result = run_tool_diagnosis(QUESTION, FakeToolDiagnosticProvider(selection(), self.valid))
        self.assertEqual(result.status, ToolRunStatus.TOOL_EXECUTION_ERROR)

    def test_historical_call_obeys_closed_arguments_identity_and_one_call_limit(self):
        messages = [selection(3), selection(name="get_order_history")]
        multiple = selection()
        multiple["tool_calls"].append(deepcopy(multiple["tool_calls"][0]))
        messages.append(multiple)
        for arguments in ('{"order_id":true}', '{"order_id":2.0}', '{"order_id":0}',
                          '{"order_id":2,"wal":"model-selected"}', '{"order_id":2,"order_id":2}'):
            message = selection()
            message["tool_calls"][0]["function"]["arguments"] = arguments
            messages.append(message)
        message = selection()
        message["tool_calls"][0]["id"] = ""
        messages.append(message)
        for message in messages:
            with self.subTest(message=message), \
                    patch("diagnostics.python.tool_diagnostic_runner.get_historical_order_evidence") as lookup:
                result = run_tool_diagnosis(QUESTION, FakeToolDiagnosticProvider(message, self.valid))
                self.assertEqual(result.status, ToolRunStatus.TOOL_CALL_INVALID)
                lookup.assert_not_called()

    def test_contradictory_facts_status_and_fake_references_are_rejected(self):
        for field, value in (("matched_quantity", 1), ("final_status", "fully_filled")):
            proposal = json.loads(self.valid)
            proposal["facts"][field] = value
            result = run_tool_diagnosis(QUESTION, FakeToolDiagnosticProvider(selection(), json.dumps(proposal)))
            self.assertEqual(result.status, RunStatus.VALIDATION_REJECTED)
            self.assertIsNone(result.to_dict()["historical_summary"])
        for kind in ("wal_sequence", "ledger_sequence"):
            proposal = json.loads(self.valid)
            proposal["evidence_refs"] = [{"kind": kind, "id": 999}]
            result = run_tool_diagnosis(QUESTION, FakeToolDiagnosticProvider(selection(), json.dumps(proposal)))
            self.assertEqual(result.status, RunStatus.VALIDATION_REJECTED)

    def test_partial_fill_response_cannot_satisfy_historical_question(self):
        partial = (FIXTURES / "valid_partial_fill_diagnosis.json").read_text()
        result = run_tool_diagnosis(QUESTION, FakeToolDiagnosticProvider(selection(), partial))
        self.assertEqual(result.status, RunStatus.PARSE_ERROR)

    def test_historical_schema_only_exposes_order_id(self):
        schema = historical_order_tool_schema()["function"]
        self.assertEqual(schema["name"], HISTORICAL_TOOL_NAME)
        self.assertEqual(schema["parameters"], {
            "type": "object", "properties": {"order_id": {"type": "integer", "minimum": 1}},
            "required": ["order_id"], "additionalProperties": False,
        })
        self.assertIn("Does not provide", schema["description"])

    def test_fake_cli_has_distinct_historical_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            selection_path = directory / "selection.json"
            final_path = directory / "final.json"
            selection_path.write_text(json.dumps(selection()))
            final_path.write_text(self.valid)
            output = io.StringIO()
            with patch("sys.argv", ["run_tool_diagnosis", QUESTION,
                                    "--evidence-source", "fixture", "--selection-file", str(selection_path),
                                    "--response-file", str(final_path)]), redirect_stdout(output):
                code = main()
        self.assertEqual(code, 0)
        result = json.loads(output.getvalue())
        self.assertEqual(result["historical_summary"]["response_type"], "historical_order_summary")
        self.assertEqual(result["selected_tool"]["name"], HISTORICAL_TOOL_NAME)

    def test_valid_lifecycle_uses_one_retrieval_and_one_final_completion(self):
        valid = (FIXTURES / "valid_historical_lifecycle_summary.json").read_text()
        provider = Mock(spec=ToolDiagnosticProvider, wraps=FakeToolDiagnosticProvider(selection(), valid))
        with patch("diagnostics.python.tool_diagnostic_runner.get_historical_order_evidence",
                   wraps=get_historical_order_evidence) as lookup:
            result = run_tool_diagnosis(QUESTION, provider,
                historical_evidence_path=FIXTURES / "d2e_historical_cancelled.json")
            lookup.assert_called_once()
        self.assertEqual(result.status, RunStatus.ACCEPTED)
        self.assertEqual(result.selected_tool.name, HISTORICAL_TOOL_NAME)
        self.assertEqual(len(result.historical_run.historical_summary.cancel_attempts), 3)
        provider.select_evidence_tool.assert_called_once()
        provider.complete_with_evidence_tool.assert_called_once()

    def test_fake_lifecycle_failures_keep_existing_statuses(self):
        original = json.loads((FIXTURES / "valid_historical_lifecycle_summary.json").read_text())
        submit = deepcopy(original)
        submit["facts"]["submission_recovered_result"] = "AccountNotFound"
        cancel = deepcopy(original)
        cancel["cancel_attempts"][0]["recovered_result"] = "Cancelled"
        missing = deepcopy(original)
        missing["cancel_attempts"].pop()
        reference = deepcopy(original)
        reference["evidence_refs"].append({"kind": "wal_sequence", "id": 999})
        for proposal, status in ((submit, RunStatus.VALIDATION_REJECTED),
                                 (cancel, RunStatus.VALIDATION_REJECTED),
                                 (missing, RunStatus.VALIDATION_REJECTED),
                                 (reference, RunStatus.VALIDATION_REJECTED),
                                 ("{", RunStatus.PARSE_ERROR)):
            raw = proposal if isinstance(proposal, str) else json.dumps(proposal)
            provider = Mock(spec=ToolDiagnosticProvider,
                            wraps=FakeToolDiagnosticProvider(selection(), raw))
            result = run_tool_diagnosis(QUESTION, provider,
                historical_evidence_path=FIXTURES / "d2e_historical_cancelled.json")
            self.assertEqual(result.status, status)
            self.assertIsNone(result.historical_run.historical_summary)
            provider.complete_with_evidence_tool.assert_called_once()

    def test_fake_business_rejection_cannot_be_relabelled_accepted(self):
        proposal = json.loads((FIXTURES / "valid_historical_rejected_summary.json").read_text())
        question = "What durable evidence do we have for order 4?"
        for result_name, status in (("InsufficientFunds", RunStatus.ACCEPTED),
                                    ("Accepted", RunStatus.VALIDATION_REJECTED)):
            proposal["facts"]["submission_recovered_result"] = result_name
            result = run_tool_diagnosis(question,
                FakeToolDiagnosticProvider(selection(4), json.dumps(proposal)),
                historical_evidence_path=FIXTURES / "d2e_historical_rejected.json")
            self.assertEqual(result.status, status)

    def test_missing_lifecycle_evidence_stops_before_final_provider_request(self):
        with tempfile.TemporaryDirectory() as temporary:
            evidence = json.loads((FIXTURES / "d2c_historical_resting.json").read_text())
            del evidence["submission"]["recovered_result"]
            path = Path(temporary) / "invalid.json"
            path.write_text(json.dumps(evidence))
            provider = Mock(spec=ToolDiagnosticProvider,
                            wraps=FakeToolDiagnosticProvider(selection(), self.valid))
            result = run_tool_diagnosis(QUESTION, provider, historical_evidence_path=path)
        self.assertEqual(result.status, ToolRunStatus.TOOL_EXECUTION_ERROR)
        provider.select_evidence_tool.assert_called_once()
        provider.complete_with_evidence_tool.assert_not_called()
