import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from diagnostics.python.partial_fill_diagnosis import (
    Cause, ErrorCategory, RemainingState, parse_diagnosis_json,
    validate_diagnosis, validate_json_documents,
)


FIXTURES = Path(__file__).parent / "fixtures"
ROOT = Path(__file__).resolve().parents[3]


class PartialFillDiagnosisTest(unittest.TestCase):
    def setUp(self):
        # Captured from exchange_diagnostic_evidence's deterministic D0 scenario.
        # Tests intentionally do not invoke C++ or require a build directory.
        self.evidence_json = (FIXTURES / "d0_partial_fill.json").read_text()
        self.evidence = json.loads(self.evidence_json)
        self.proposal = json.loads(
            (FIXTURES / "valid_partial_fill_diagnosis.json").read_text()
        )

    def validate(self, proposal=None, evidence=None):
        return validate_json_documents(
            json.dumps(self.evidence if evidence is None else evidence),
            json.dumps(self.proposal if proposal is None else proposal),
        )

    def assert_error(self, result, category, path=None):
        self.assertFalse(result.valid)
        self.assertTrue(any(
            error.category == category and (path is None or error.path == path)
            for error in result.errors
        ), result.to_dict())

    def test_valid_diagnosis_has_typed_parse_then_grounding(self):
        parsed = parse_diagnosis_json(json.dumps(self.proposal))
        self.assertEqual(parsed.errors, ())
        self.assertEqual(parsed.diagnosis.cause,
                         Cause.INSUFFICIENT_EXECUTABLE_LIQUIDITY)
        self.assertEqual(parsed.diagnosis.facts.remaining_state, RemainingState.RESTING)
        result = validate_diagnosis(parsed.diagnosis, self.evidence)
        self.assertEqual(result.to_dict(), {"valid": True, "errors": []})

    def test_wrong_numeric_facts_are_contradictions(self):
        for field, wrong_value in {
            "order_id": 2, "submitted_quantity": 6, "matched_quantity": 4,
            "remaining_quantity": 1, "limit_price": 101,
        }.items():
            with self.subTest(field=field):
                proposal = copy.deepcopy(self.proposal)
                proposal["facts"][field] = wrong_value
                self.assert_error(self.validate(proposal),
                                  ErrorCategory.CONTRADICTORY_FACT, f"facts.{field}")

    def test_filled_and_cancelled_contradict_resting(self):
        for state in ("filled", "cancelled"):
            with self.subTest(state=state):
                self.proposal["facts"]["remaining_state"] = state
                self.assert_error(self.validate(), ErrorCategory.CONTRADICTORY_FACT,
                                  "facts.remaining_state")

    def test_false_resting_contradicts_resting_claim(self):
        self.evidence["post_execution"]["resting"] = False
        self.assert_error(self.validate(), ErrorCategory.CONTRADICTORY_FACT,
                          "facts.remaining_state")

    def test_absent_resting_order_does_not_prove_filled_or_cancelled(self):
        self.evidence["post_execution"]["resting"] = False
        for state in ("filled", "cancelled"):
            with self.subTest(state=state):
                self.proposal["facts"]["remaining_state"] = state
                self.assert_error(self.validate(), ErrorCategory.UNSUPPORTED_FACT,
                                  "facts.remaining_state")

    def test_fake_references_are_rejected(self):
        for kind in ("order", "ledger_sequence", "wal_sequence"):
            with self.subTest(kind=kind):
                self.proposal["evidence_refs"] = [{"kind": kind, "id": 999}]
                self.assert_error(self.validate(), ErrorCategory.INVALID_EVIDENCE_REFERENCE)

    def test_reference_kinds_do_not_share_id_spaces(self):
        for kind, identity in (
            ("order", 1042),  # RequestId is not an OrderId.
            ("order", 4),  # LedgerSequence is not an OrderId.
            ("ledger_sequence", 3),  # WAL/order sequence is not a ledger sequence.
            ("wal_sequence", 4),  # Ledger sequence is not a WAL sequence.
        ):
            with self.subTest(kind=kind, identity=identity):
                self.proposal["evidence_refs"] = [{"kind": kind, "id": identity}]
                self.assert_error(self.validate(), ErrorCategory.INVALID_EVIDENCE_REFERENCE)

    def test_opposing_order_reference_resolves(self):
        self.proposal["evidence_refs"] = [{"kind": "order", "id": 2}]
        self.assertTrue(self.validate().valid)

    def test_unknown_reference_kind_is_schema_error(self):
        self.proposal["evidence_refs"] = [{"kind": "trade_id", "id": 1}]
        self.assert_error(self.validate(), ErrorCategory.SCHEMA_ERROR)

    def test_unknown_cause_is_schema_error(self):
        self.proposal["cause"] = "market_manipulation"
        self.assert_error(self.validate(), ErrorCategory.SCHEMA_ERROR)

    def test_known_cause_requires_each_evidence_relation(self):
        changes = (
            ("submitted", lambda e: e["target_order"].update(submitted_quantity=2)),
            ("liquidity", lambda e: e["pre_execution_book"].update(total_executable_quantity=4)),
            ("remaining", lambda e: e["post_execution"].update(remaining_quantity=0)),
            ("conservation", lambda e: e["post_execution"].update(remaining_quantity=4)),
        )
        for name, mutate in changes:
            with self.subTest(relation=name):
                evidence = copy.deepcopy(self.evidence)
                mutate(evidence)
                self.assert_error(self.validate(evidence=evidence),
                                  ErrorCategory.UNSUPPORTED_CAUSE, "cause")

    def test_abstention_is_valid_and_does_not_claim_a_cause(self):
        self.proposal["cause"] = "unsupported_by_evidence"
        self.assertTrue(self.validate().valid)
        # Lack of a cause relation is permitted; factual grounding still applies.
        self.evidence["pre_execution_book"]["total_executable_quantity"] = 4
        self.assertTrue(self.validate().valid)

    def test_abstention_cannot_bypass_facts_or_references(self):
        self.proposal["cause"] = "unsupported_by_evidence"
        self.proposal["facts"]["matched_quantity"] = 4
        self.proposal["evidence_refs"] = [{"kind": "wal_sequence", "id": 999}]
        result = self.validate()
        self.assert_error(result, ErrorCategory.CONTRADICTORY_FACT)
        self.assert_error(result, ErrorCategory.INVALID_EVIDENCE_REFERENCE)

    def test_abstention_cannot_bypass_malformed_schema(self):
        self.proposal["cause"] = "unsupported_by_evidence"
        del self.proposal["facts"]["order_id"]
        self.assert_error(self.validate(), ErrorCategory.SCHEMA_ERROR)

    def test_schema_rejects_missing_extra_fields_and_wrong_types(self):
        changes = (
            lambda p: p.update(diagnosis_type="full_fill"),
            lambda p: p.update(confidence=1),
            lambda p: p.pop("summary"),
            lambda p: p.update(summary=" "),
            lambda p: p.update(summary=123),
            lambda p: p.update(facts=[]),
            lambda p: p["facts"].update(extra=1),
            lambda p: p["facts"].update(order_id=True),
            lambda p: p["facts"].update(matched_quantity=2.0),
            lambda p: p["facts"].update(remaining_quantity=-1),
            lambda p: p["facts"].update(limit_price="100"),
            lambda p: p["facts"].update(remaining_state="unknown"),
            lambda p: p.update(evidence_refs=[]),
            lambda p: p.update(evidence_refs={"kind": "order", "id": 3}),
            lambda p: p["evidence_refs"][0].update(id=False),
            lambda p: p["evidence_refs"][0].update(id=0),
            lambda p: p["evidence_refs"][0].update(extra=1),
        )
        for index, mutate in enumerate(changes):
            with self.subTest(case=index):
                proposal = copy.deepcopy(self.proposal)
                mutate(proposal)
                self.assert_error(self.validate(proposal), ErrorCategory.SCHEMA_ERROR)

    def test_malformed_diagnosis_json_is_schema_error(self):
        for raw in ("{", "[]", "null", '{"a": 1, "a": 2}',
                    json.dumps(self.proposal).replace('"limit_price": 100',
                                                      '"limit_price": NaN')):
            with self.subTest(raw=raw):
                self.assert_error(validate_json_documents(self.evidence_json, raw),
                                  ErrorCategory.SCHEMA_ERROR)

    def test_missing_or_mistyped_evidence_is_unsupported_not_a_contradiction(self):
        for value in (None, True, 2.0, "2", -1):
            with self.subTest(value=value):
                evidence = copy.deepcopy(self.evidence)
                evidence["matched_quantity"] = value
                self.assert_error(self.validate(evidence=evidence),
                                  ErrorCategory.UNSUPPORTED_FACT, "facts.matched_quantity")
        del self.evidence["post_execution"]
        self.assert_error(self.validate(), ErrorCategory.UNSUPPORTED_FACT,
                          "facts.remaining_state")

    def test_invalid_evidence_json_returns_structured_error(self):
        for raw in ("{", "[]", "null", '{"case_type":"other"}',
                    '{"case_type":"partial_fill","case_type":"partial_fill"}'):
            with self.subTest(raw=raw):
                self.assert_error(validate_json_documents(raw, json.dumps(self.proposal)),
                                  ErrorCategory.SCHEMA_ERROR)

    def test_summary_prose_is_not_judged(self):
        self.proposal["summary"] = "This prose is not semantically validated."
        self.assertTrue(self.validate().valid)


class DiagnosisCliTest(unittest.TestCase):
    def run_cli(self, evidence, diagnosis):
        return subprocess.run(
            [sys.executable, "-m", "diagnostics.python.validate_diagnosis",
             str(evidence), str(diagnosis)],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )

    def test_valid_fixture_cli(self):
        result = self.run_cli(FIXTURES / "d0_partial_fill.json",
                              FIXTURES / "valid_partial_fill_diagnosis.json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), {"valid": True, "errors": []})

    def test_rejected_cli_has_nonzero_exit_and_structured_error(self):
        with tempfile.TemporaryDirectory() as directory:
            diagnosis = Path(directory) / "diagnosis.json"
            proposal = json.loads((FIXTURES / "valid_partial_fill_diagnosis.json").read_text())
            proposal["facts"]["matched_quantity"] = 4
            diagnosis.write_text(json.dumps(proposal))
            result = self.run_cli(FIXTURES / "d0_partial_fill.json", diagnosis)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(json.loads(result.stdout)["errors"][0]["category"],
                         "contradictory_fact")

    def test_unreadable_file_returns_structured_error(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_cli(Path(directory) / "missing.json",
                                  FIXTURES / "valid_partial_fill_diagnosis.json")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(json.loads(result.stdout)["errors"][0]["category"], "schema_error")


if __name__ == "__main__":
    unittest.main()
