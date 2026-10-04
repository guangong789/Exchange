import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock

from diagnostics.python.diagnostic_provider import (
    DiagnosticProvider, FakeDiagnosticProvider, ProviderError, ProviderTimeoutError,
)
from diagnostics.python.diagnostic_runner import (
    RunStatus, build_partial_fill_prompt, run_diagnosis,
)
from diagnostics.python.partial_fill_diagnosis import (
    ErrorCategory, parse_diagnosis_json, validate_diagnosis,
)


FIXTURES = Path(__file__).parent / "fixtures"
ROOT = Path(__file__).resolve().parents[3]


class DiagnosticRunnerTest(unittest.TestCase):
    def setUp(self):
        self.evidence_json = (FIXTURES / "d0_partial_fill.json").read_text()
        self.valid_json = (FIXTURES / "valid_partial_fill_diagnosis.json").read_text()

    def run_raw(self, raw):
        return run_diagnosis(self.evidence_json, FakeDiagnosticProvider(raw))

    def assert_failure(self, result, status):
        self.assertEqual(result.status, status)
        self.assertIsNone(result.diagnosis)

    def test_valid_response_returns_the_existing_typed_diagnosis(self):
        result = self.run_raw(self.valid_json)
        self.assertEqual(result.status, RunStatus.ACCEPTED)
        self.assertEqual(result.diagnosis, parse_diagnosis_json(self.valid_json).diagnosis)
        self.assertEqual(result.errors, ())
        self.assertEqual(result.raw_provider_output, self.valid_json)
        # The local structured result is directly JSON-serializable.
        output = json.loads(json.dumps(result.to_dict()))
        self.assertEqual(output["diagnosis"], json.loads(self.valid_json))

    def test_malformed_response_is_parse_error(self):
        raw = '{"diagnosis_type":'
        result = self.run_raw(raw)
        self.assert_failure(result, RunStatus.PARSE_ERROR)
        self.assertEqual(result.errors, parse_diagnosis_json(raw).errors)
        self.assertEqual(result.raw_provider_output, raw)

    def test_schema_invalid_response_is_parse_error(self):
        proposal = json.loads(self.valid_json)
        del proposal["facts"]["remaining_quantity"]
        raw = json.dumps(proposal)
        result = self.run_raw(raw)
        self.assert_failure(result, RunStatus.PARSE_ERROR)
        self.assertEqual(result.errors, parse_diagnosis_json(raw).errors)
        self.assertEqual(result.errors[0].category, ErrorCategory.SCHEMA_ERROR)

    def test_contradiction_preserves_d1a_validation_errors(self):
        proposal = json.loads(self.valid_json)
        proposal["facts"]["matched_quantity"] = 4
        raw = json.dumps(proposal)
        result = self.run_raw(raw)
        self.assert_failure(result, RunStatus.VALIDATION_REJECTED)
        expected = validate_diagnosis(
            parse_diagnosis_json(raw).diagnosis, json.loads(self.evidence_json),
        )
        self.assertEqual(result.errors, expected.errors)
        self.assertEqual(result.errors[0].category, ErrorCategory.CONTRADICTORY_FACT)
        self.assertEqual(result.raw_provider_output, raw)

    def test_fake_ledger_reference_is_validation_rejected(self):
        proposal = json.loads(self.valid_json)
        proposal["evidence_refs"] = [{"kind": "ledger_sequence", "id": 999}]
        result = self.run_raw(json.dumps(proposal))
        self.assert_failure(result, RunStatus.VALIDATION_REJECTED)
        self.assertEqual(result.errors[0].category, ErrorCategory.INVALID_EVIDENCE_REFERENCE)

    def test_unknown_reference_kind_stays_parse_error(self):
        proposal = json.loads(self.valid_json)
        proposal["evidence_refs"] = [{"kind": "trade_id", "id": 1}]
        self.assert_failure(self.run_raw(json.dumps(proposal)), RunStatus.PARSE_ERROR)

    def test_declared_timeout_is_distinct_from_generic_provider_failure(self):
        for failure, status in (
            (ProviderTimeoutError("simulated timeout"), RunStatus.PROVIDER_TIMEOUT),
            (ProviderError("simulated failure"), RunStatus.PROVIDER_ERROR),
        ):
            with self.subTest(status=status):
                result = run_diagnosis(
                    self.evidence_json, FakeDiagnosticProvider(failure=failure),
                )
                self.assert_failure(result, status)
                self.assertIsNone(result.raw_provider_output)
                self.assertEqual(result.errors, ())
                self.assertNotIn(str(failure), json.dumps(result.to_dict()))

    def test_exactly_one_provider_call_without_retries(self):
        for response, failure, status in (
            (self.valid_json, None, RunStatus.ACCEPTED),
            ("{", None, RunStatus.PARSE_ERROR),
            (None, ProviderTimeoutError(), RunStatus.PROVIDER_TIMEOUT),
            (None, ProviderError(), RunStatus.PROVIDER_ERROR),
        ):
            with self.subTest(status=status):
                provider = Mock(spec=DiagnosticProvider)
                provider.complete.return_value = response
                provider.complete.side_effect = failure
                result = run_diagnosis(self.evidence_json, provider)
                self.assertEqual(result.status, status)
                provider.complete.assert_called_once_with(
                    build_partial_fill_prompt(self.evidence_json),
                )

    def test_unexpected_internal_exceptions_propagate(self):
        provider = Mock(spec=DiagnosticProvider)
        for failure in (RuntimeError("bug"), OSError("internal failure")):
            with self.subTest(failure=type(failure).__name__):
                provider.complete.side_effect = failure
                with self.assertRaises(type(failure)) as raised:
                    run_diagnosis(self.evidence_json, provider)
                self.assertIs(raised.exception, failure)

    def test_malformed_or_wrong_case_evidence_never_calls_provider(self):
        for raw in ("{", "null", "[]", "{}", '{"case_type":"other"}',
                    '{"case_type":"partial_fill","case_type":"partial_fill"}',
                    '{"case_type":"partial_fill","matched_quantity":NaN}'):
            with self.subTest(raw=raw):
                provider = Mock(spec=DiagnosticProvider)
                result = run_diagnosis(raw, provider)
                self.assert_failure(result, RunStatus.VALIDATION_REJECTED)
                self.assertEqual(result.errors[0].category, ErrorCategory.SCHEMA_ERROR)
                self.assertEqual(result.errors[0].path, "evidence")
                self.assertIsNone(result.raw_provider_output)
                provider.complete.assert_not_called()

    def test_missing_evidence_facts_use_existing_grounding_errors(self):
        evidence = json.loads(self.evidence_json)
        del evidence["post_execution"]
        result = run_diagnosis(json.dumps(evidence), FakeDiagnosticProvider(self.valid_json))
        self.assert_failure(result, RunStatus.VALIDATION_REJECTED)
        self.assertTrue(any(error.category == ErrorCategory.UNSUPPORTED_FACT
                            for error in result.errors))

    def test_prompt_is_deterministic_and_contains_the_supplied_evidence(self):
        prompt = build_partial_fill_prompt(self.evidence_json)
        self.assertEqual(prompt, build_partial_fill_prompt(self.evidence_json))
        self.assertIn(self.evidence_json, prompt)
        for fact in ('"order_id": 3', '"matched_quantity": 2',
                     '"submitted_quantity": 5', '"wal_sequence": 3',
                     '"ledger_sequence": 4'):
            self.assertIn(fact, prompt)

    def test_prompt_has_grounding_and_schema_constraints(self):
        prompt = build_partial_fill_prompt(self.evidence_json)
        for instruction in (
            "Use only the supplied evidence", "Do not invent facts",
            "Return only the required structured JSON diagnosis",
            "unsupported_by_evidence", "evidence_refs", "remaining_state",
            "insufficient_executable_liquidity", "No floats or booleans",
        ):
            self.assertIn(instruction, prompt)

    def test_fake_returns_raw_text_without_parsing(self):
        raw = "not even JSON"
        self.assertEqual(FakeDiagnosticProvider(raw).complete("ignored prompt"), raw)

    def test_summary_remains_display_only(self):
        proposal = json.loads(self.valid_json)
        proposal["summary"] = "This text is not semantically checked."
        self.assertEqual(self.run_raw(json.dumps(proposal)).status, RunStatus.ACCEPTED)

    def test_abstention_still_uses_d1a_fact_validation(self):
        proposal = json.loads(self.valid_json)
        proposal["cause"] = "unsupported_by_evidence"
        self.assertEqual(self.run_raw(json.dumps(proposal)).status, RunStatus.ACCEPTED)
        proposal["facts"]["remaining_quantity"] = 1
        self.assert_failure(self.run_raw(json.dumps(proposal)), RunStatus.VALIDATION_REJECTED)


class DiagnosticRunnerCliTest(unittest.TestCase):
    def run_cli(self, *arguments):
        return subprocess.run(
            [sys.executable, "-m", "diagnostics.python.run_diagnosis", *map(str, arguments)],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )

    def test_valid_response_file_cli(self):
        result = self.run_cli(FIXTURES / "d0_partial_fill.json", "--response-file",
                              FIXTURES / "valid_partial_fill_diagnosis.json")
        self.assertEqual(result.returncode, 0, result.stderr)
        output = json.loads(result.stdout)
        self.assertEqual(output["status"], "accepted")
        self.assertEqual(output["diagnosis"]["facts"]["order_id"], 3)

    def test_fake_failure_cli(self):
        for failure in ("timeout", "error"):
            with self.subTest(failure=failure):
                result = self.run_cli(FIXTURES / "d0_partial_fill.json", "--failure", failure)
                self.assertEqual(result.returncode, 1, result.stderr)
                output = json.loads(result.stdout)
                self.assertEqual(output["status"], f"provider_{failure}")
                self.assertIsNone(output["diagnosis"])
                self.assertIsNone(output["raw_provider_output"])

    def test_malformed_response_file_cli(self):
        with tempfile.TemporaryDirectory() as directory:
            response = Path(directory) / "response.txt"
            response.write_text("{", encoding="utf-8")
            result = self.run_cli(FIXTURES / "d0_partial_fill.json", "--response-file", response)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(json.loads(result.stdout)["status"], "parse_error")

    def test_missing_input_is_not_provider_error(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_cli(Path(directory) / "missing.json", "--failure", "error")
        self.assertEqual(result.returncode, 1, result.stderr)
        output = json.loads(result.stdout)
        self.assertEqual(output["status"], "validation_rejected")
        self.assertEqual(output["errors"][0]["path"], "input")


if __name__ == "__main__":
    unittest.main()
