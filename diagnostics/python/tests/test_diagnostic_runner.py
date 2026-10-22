from dataclasses import replace
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

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
CANONICAL_SUMMARY = (
    "In the fixed D0 execution-time scenario, order 3 submitted 5 units at limit price 100; "
    "2 units matched and 3 remain, with remaining state resting. "
    "The execution-time evidence supports insufficient executable liquidity at the limit price."
)


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
        parsed = parse_diagnosis_json(self.valid_json).diagnosis
        self.assertEqual(result.diagnosis, replace(parsed, summary=CANONICAL_SUMMARY))
        self.assertEqual(parsed.summary, json.loads(self.valid_json)["summary"])
        self.assertEqual(result.errors, ())
        self.assertEqual(result.raw_provider_output, self.valid_json)
        # The local structured result is directly JSON-serializable.
        output = json.loads(json.dumps(result.to_dict()))
        expected = json.loads(self.valid_json)
        expected["summary"] = CANONICAL_SUMMARY
        self.assertEqual(output["diagnosis"], expected)

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

    def test_deeply_nested_completion_stops_before_grounding_and_rendering(self):
        raw = "[" * 2000 + "0" + "]" * 2000
        provider = Mock(spec=DiagnosticProvider, wraps=FakeDiagnosticProvider(raw))
        with patch("diagnostics.python.diagnostic_runner.validate_diagnosis") as validate, \
                patch("diagnostics.python.diagnostic_runner.render_canonical_partial_fill_summary") as render:
            result = run_diagnosis(self.evidence_json, provider)
            validate.assert_not_called()
            render.assert_not_called()
        self.assert_failure(result, RunStatus.PARSE_ERROR)
        self.assertEqual(result.errors[0].category, ErrorCategory.SCHEMA_ERROR)
        self.assertEqual(result.raw_provider_output, raw)
        provider.complete.assert_called_once()

    def test_recursion_errors_outside_model_parsing_still_propagate(self):
        for operation in ("validate_diagnosis", "render_canonical_partial_fill_summary"):
            with self.subTest(operation=operation), \
                    patch(f"diagnostics.python.diagnostic_runner.{operation}",
                          side_effect=RecursionError("internal defect")):
                with self.assertRaisesRegex(RecursionError, "internal defect"):
                    self.run_raw(self.valid_json)
        provider = Mock(spec=DiagnosticProvider)
        provider.complete.side_effect = RecursionError("provider defect")
        with self.assertRaisesRegex(RecursionError, "provider defect"):
            run_diagnosis(self.evidence_json, provider)
        provider.complete.assert_called_once()

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

    def test_reference_membership_does_not_imply_coverage_or_change_canonical_prose(self):
        for cause in ("insufficient_executable_liquidity", "unsupported_by_evidence"):
            proposal = json.loads(self.valid_json)
            proposal["cause"] = cause
            baseline = self.run_raw(json.dumps(proposal)).diagnosis
            ordinary = proposal["evidence_refs"]
            for refs in ([{"kind": "order", "id": 2}],
                         [ref for ref in ordinary if ref["kind"] != "ledger_sequence"],
                         [ordinary[0], ordinary[0]]):
                with self.subTest(cause=cause, refs=refs):
                    proposal["evidence_refs"] = refs
                    proposal["summary"] = "The client received success and all funds were released."
                    raw = json.dumps(proposal)
                    result = self.run_raw(raw)
                    self.assertEqual(result.status, RunStatus.ACCEPTED)
                    self.assertEqual(result.diagnosis.facts, baseline.facts)
                    self.assertEqual(result.diagnosis.summary, baseline.summary)
                    self.assertEqual(json.loads(json.dumps(result.to_dict()))["diagnosis"]["evidence_refs"], refs)
                    self.assertEqual(result.raw_provider_output, raw)

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

    def test_arbitrary_model_prose_cannot_change_trusted_summary(self):
        summaries = (
            json.loads(self.valid_json)["summary"],
            "The client definitely received Accepted.",
            "Order 3 was cancelled and all reserved funds were released.",
            "A network outage caused the partial fill.",
        )
        trusted_summaries = []
        for summary in summaries:
            with self.subTest(summary=summary):
                proposal = json.loads(self.valid_json)
                proposal["summary"] = summary
                raw = json.dumps(proposal)
                result = self.run_raw(raw)
                self.assertEqual(result.status, RunStatus.ACCEPTED)
                self.assertEqual(result.diagnosis.summary, CANONICAL_SUMMARY)
                self.assertNotIn(summary, result.diagnosis.summary)
                self.assertEqual(result.raw_provider_output, raw)
                self.assertEqual(result.to_dict()["raw_provider_output"], raw)
                trusted_summaries.append(result.diagnosis.summary)
        self.assertEqual(trusted_summaries, [CANONICAL_SUMMARY] * len(summaries))

    def test_abstention_does_not_assert_a_cause_or_evidence_inadequacy(self):
        proposal = json.loads(self.valid_json)
        proposal["cause"] = "unsupported_by_evidence"
        proposal["summary"] = "A network outage definitely caused the partial fill."
        raw = json.dumps(proposal)
        # The unchanged D0 evidence can support the known cause; abstention is
        # still valid and must not be rendered as proof of inadequate evidence.
        result = self.run_raw(raw)
        self.assertEqual(result.status, RunStatus.ACCEPTED)
        self.assertEqual(result.diagnosis.summary, (
            "In the fixed D0 execution-time scenario, order 3 submitted 5 units at limit price 100; "
            "2 units matched and 3 remain, with remaining state resting. "
            "No supported cause is asserted in this diagnosis."
        ))
        self.assertEqual(result.raw_provider_output, raw)

    def test_invalid_claims_never_reach_renderer(self):
        cases = (
            ("facts", "matched_quantity", 4, RunStatus.VALIDATION_REJECTED),
            ("facts", "remaining_quantity", 1, RunStatus.VALIDATION_REJECTED),
            ("facts", "remaining_state", "cancelled", RunStatus.VALIDATION_REJECTED),
            ("root", "cause", "market_manipulation", RunStatus.PARSE_ERROR),
            ("root", "evidence_refs", [{"kind": "ledger_sequence", "id": 999}],
             RunStatus.VALIDATION_REJECTED),
            ("root", "evidence_refs", [{"kind": "trade_id", "id": 4}], RunStatus.PARSE_ERROR),
            ("root", "unexpected", True, RunStatus.PARSE_ERROR),
            ("root", "summary", "", RunStatus.PARSE_ERROR),
        )
        with patch("diagnostics.python.diagnostic_runner.render_canonical_partial_fill_summary") as render:
            for location, field, value, status in cases:
                with self.subTest(location=location, field=field):
                    proposal = json.loads(self.valid_json)
                    proposal["summary"] = CANONICAL_SUMMARY
                    target = proposal if location == "root" else proposal[location]
                    target[field] = value
                    self.assert_failure(self.run_raw(json.dumps(proposal)), status)
            proposal = json.loads(self.valid_json)
            del proposal["summary"]
            self.assert_failure(self.run_raw(json.dumps(proposal)), RunStatus.PARSE_ERROR)
            self.assert_failure(self.run_raw("{"), RunStatus.PARSE_ERROR)
            evidence = json.loads(self.evidence_json)
            evidence["pre_execution_book"]["total_executable_quantity"] = 4
            result = run_diagnosis(json.dumps(evidence), FakeDiagnosticProvider(self.valid_json))
            self.assert_failure(result, RunStatus.VALIDATION_REJECTED)
            self.assertTrue(any(error.category == ErrorCategory.UNSUPPORTED_CAUSE for error in result.errors))
            render.assert_not_called()

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
        self.assertEqual(output["diagnosis"]["summary"], CANONICAL_SUMMARY)

    def test_cli_isolates_fabricated_prose_in_raw_output(self):
        proposal = json.loads((FIXTURES / "valid_partial_fill_diagnosis.json").read_text())
        proposal["summary"] = "The client received Accepted; the order was cancelled and funds released."
        proposal["evidence_refs"] = [{"kind": "order", "id": 2}]
        raw = json.dumps(proposal)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "response.json"
            path.write_text(raw)
            result = self.run_cli(FIXTURES / "d0_partial_fill.json", "--response-file", path)
        self.assertEqual(result.returncode, 0, result.stderr)
        output = json.loads(result.stdout)
        self.assertEqual(output["status"], "accepted")
        self.assertEqual(output["diagnosis"]["summary"], CANONICAL_SUMMARY)
        self.assertEqual(output["diagnosis"]["evidence_refs"], proposal["evidence_refs"])
        self.assertEqual(output["raw_provider_output"], raw)

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
        output = json.loads(result.stdout)
        self.assertEqual(output["status"], "parse_error")
        self.assertIsNone(output["diagnosis"])

    def test_missing_input_is_not_provider_error(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_cli(Path(directory) / "missing.json", "--failure", "error")
        self.assertEqual(result.returncode, 1, result.stderr)
        output = json.loads(result.stdout)
        self.assertEqual(output["status"], "validation_rejected")
        self.assertEqual(output["errors"][0]["path"], "input")


if __name__ == "__main__":
    unittest.main()
