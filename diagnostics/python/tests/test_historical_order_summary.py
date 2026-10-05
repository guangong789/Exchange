from copy import deepcopy
import json
from pathlib import Path
import unittest
from unittest.mock import Mock

from diagnostics.python.diagnostic_provider import FakeDiagnosticProvider, ProviderError, ProviderTimeoutError
from diagnostics.python.diagnostic_runner import RunStatus
from diagnostics.python.historical_order_summary import parse_historical_summary_json, validate_historical_summary
from diagnostics.python.historical_summary_runner import build_historical_summary_prompt, run_historical_summary
from diagnostics.python.historical_replay_result import ReplayResult


FIXTURES = Path(__file__).parent / "fixtures"


class HistoricalSummaryTest(unittest.TestCase):
    def setUp(self):
        self.evidence = json.loads((FIXTURES / "d2c_historical_resting.json").read_text())
        self.proposal = json.loads((FIXTURES / "valid_historical_order_summary.json").read_text())

    def run_proposal(self, proposal=None, evidence=None):
        return run_historical_summary(json.dumps(self.evidence if evidence is None else evidence),
                                      FakeDiagnosticProvider(json.dumps(self.proposal if proposal is None else proposal)))

    def test_typed_resting_summary_is_grounded(self):
        parsed = parse_historical_summary_json(json.dumps(self.proposal))
        self.assertEqual(parsed.errors, ())
        self.assertTrue(validate_historical_summary(parsed.summary, self.evidence).valid)
        result = self.run_proposal()
        self.assertEqual(result.status, RunStatus.ACCEPTED)
        self.assertEqual(result.historical_summary.facts.remaining_quantity, 2)
        self.assertNotIn("cause", result.to_dict()["historical_summary"])

    def test_fully_filled_and_unavailable_remaining_are_distinct(self):
        filled = json.loads((FIXTURES / "d2c_historical_filled.json").read_text())
        proposal = deepcopy(self.proposal)
        proposal["facts"].update(order_id=1, submitted_quantity=2, submitted_side="SELL",
                                  matched_quantity=2, final_status="fully_filled", remaining_quantity=0)
        proposal["evidence_refs"] = [{"kind": "wal_sequence", "id": 1}, {"kind": "ledger_sequence", "id": 3}]
        self.assertEqual(self.run_proposal(proposal, filled).status, RunStatus.ACCEPTED)
        neutral = deepcopy(self.evidence)
        neutral["final_state"].update(status="not_resting", remaining_quantity=None,
                                      reservation=None, side=None, price=None)
        proposal = deepcopy(self.proposal)
        proposal["facts"].update(final_status="not_resting", remaining_quantity=None)
        self.assertEqual(self.run_proposal(proposal, neutral).status, RunStatus.ACCEPTED)
        proposal["facts"]["remaining_quantity"] = 0
        self.assertEqual(self.run_proposal(proposal, neutral).status, RunStatus.VALIDATION_REJECTED)
        proposal["facts"]["remaining_quantity"] = None
        del neutral["final_state"]["remaining_quantity"]
        self.assertEqual(self.run_proposal(proposal, neutral).status, RunStatus.VALIDATION_REJECTED)

    def test_each_claim_is_checked_against_its_evidence_field(self):
        for field, value in (("order_id", 3), ("submitted_quantity", 6), ("submitted_price", 101),
                             ("submitted_side", "SELL"), ("matched_quantity", 4),
                             ("remaining_quantity", 1), ("final_status", "fully_filled")):
            with self.subTest(field=field):
                proposal = deepcopy(self.proposal)
                proposal["facts"][field] = value
                result = self.run_proposal(proposal)
                self.assertEqual(result.status, RunStatus.VALIDATION_REJECTED)
                self.assertEqual(result.errors[0].category, "contradictory_fact")
                self.assertIsNone(result.historical_summary)

    def test_reference_namespaces_use_only_submission_wal_and_returned_trades(self):
        for kind, identifier in (("wal_sequence", 4), ("wal_sequence", 2002),
                                 ("ledger_sequence", 2), ("ledger_sequence", 999)):
            with self.subTest(kind=kind, identifier=identifier):
                proposal = deepcopy(self.proposal)
                proposal["evidence_refs"] = [{"kind": kind, "id": identifier}]
                result = self.run_proposal(proposal)
                self.assertEqual(result.status, RunStatus.VALIDATION_REJECTED)
                self.assertEqual(result.errors[0].category, "invalid_evidence_reference")

    def test_unavailable_and_causal_fields_are_outside_the_closed_schema(self):
        for field in ("response_delivery", "pre_execution_book", "matching_stop_reason", "cause"):
            for location in ("root", "facts"):
                with self.subTest(field=field, location=location):
                    proposal = deepcopy(self.proposal)
                    target = proposal if location == "root" else proposal["facts"]
                    target[field] = "invented certainty"
                    self.assertEqual(self.run_proposal(proposal).status, RunStatus.PARSE_ERROR)

    def test_model_prose_is_never_promoted_to_trusted_causality(self):
        self.proposal["summary"] = "Insufficient liquidity stopped matching and the client received success."
        result = self.run_proposal()
        self.assertEqual(result.status, RunStatus.ACCEPTED)
        self.assertNotIn("Insufficient liquidity", result.historical_summary.summary)
        self.assertNotIn("received success", result.historical_summary.summary)
        self.assertIn("does not establish", result.historical_summary.summary)
        self.assertIn("Insufficient liquidity", result.raw_provider_output)

    def test_invalid_types_unknown_keys_and_reference_kinds_are_parse_errors(self):
        for field, value in (("order_id", True), ("submitted_quantity", 5.0),
                             ("matched_quantity", "3"), ("remaining_quantity", False),
                             ("submitted_price", 0), ("final_status", "cancelled")):
            proposal = deepcopy(self.proposal)
            proposal["facts"][field] = value
            self.assertEqual(self.run_proposal(proposal).status, RunStatus.PARSE_ERROR)
        for kind in ("order", "trade_id", "request_id"):
            proposal = deepcopy(self.proposal)
            proposal["evidence_refs"] = [{"kind": kind, "id": 2}]
            self.assertEqual(self.run_proposal(proposal).status, RunStatus.PARSE_ERROR)
        for raw in ("{", '{"response_type":"x","response_type":"y"}',
                    '{"facts":NaN}', "[" * 2000):
            self.assertTrue(parse_historical_summary_json(raw).errors)

    def test_missing_or_invalid_evidence_facts_are_not_grounded(self):
        for section in ("submission", "matched_quantity", "final_state"):
            evidence = deepcopy(self.evidence)
            del evidence[section]
            self.assertEqual(self.run_proposal(evidence=evidence).status, RunStatus.VALIDATION_REJECTED)
        evidence = deepcopy(self.evidence)
        evidence["matched_quantity"] = True
        self.assertEqual(self.run_proposal(evidence=evidence).status, RunStatus.VALIDATION_REJECTED)

    def test_schema_is_explicitly_separate_from_partial_fill(self):
        partial = (FIXTURES / "valid_partial_fill_diagnosis.json").read_text()
        self.assertTrue(parse_historical_summary_json(partial).errors)
        provider = Mock()
        partial_evidence = (FIXTURES / "d0_partial_fill.json").read_text()
        result = run_historical_summary(partial_evidence, provider)
        self.assertEqual(result.status, RunStatus.VALIDATION_REJECTED)
        provider.complete.assert_not_called()

    def test_provider_failures_are_one_attempt_and_internal_errors_propagate(self):
        for failure, status in ((ProviderError(), RunStatus.PROVIDER_ERROR),
                                (ProviderTimeoutError(), RunStatus.PROVIDER_TIMEOUT)):
            provider = Mock()
            provider.complete.side_effect = failure
            result = run_historical_summary(json.dumps(self.evidence), provider)
            self.assertEqual(result.status, status)
            provider.complete.assert_called_once()
        provider.complete.reset_mock()
        provider.complete.side_effect = RuntimeError("internal bug")
        with self.assertRaises(RuntimeError):
            run_historical_summary(json.dumps(self.evidence), provider)

    def test_prompt_contains_scope_and_no_cause_taxonomy(self):
        prompt = build_historical_summary_prompt(json.dumps(self.evidence))
        for required in ("historical_order_summary", "submission.wal_sequence", "trades[].ledger_sequence",
                         "response delivery", "No diagnosis cause", "No floats or booleans"):
            self.assertIn(required, prompt)
        self.assertNotIn("insufficient_executable_liquidity", prompt)

    def lifecycle(self):
        return (json.loads((FIXTURES / "d2e_historical_cancelled.json").read_text()),
                json.loads((FIXTURES / "valid_historical_lifecycle_summary.json").read_text()))

    def rejected(self):
        return (json.loads((FIXTURES / "d2e_historical_rejected.json").read_text()),
                json.loads((FIXTURES / "valid_historical_rejected_summary.json").read_text()))

    def test_closed_result_vocabulary_matches_cpp_names(self):
        self.assertEqual({result.value for result in ReplayResult}, {
            "Accepted", "Cancelled", "AccountNotFound", "InsufficientFunds",
            "DuplicateOrder", "InvalidOrder", "CounterpartyNotAccountBacked",
            "CancelNotFound", "CancelNotOwner", "InvalidRequest",
        })

    def test_lifecycle_claims_and_canonical_prose_preserve_each_attempt(self):
        evidence, proposal = self.lifecycle()
        result = self.run_proposal(proposal, evidence)
        self.assertEqual(result.status, RunStatus.ACCEPTED)
        summary = result.historical_summary
        self.assertEqual(summary.facts.submission_recovered_result, ReplayResult.ACCEPTED)
        self.assertEqual(summary.facts.outcome_basis, "deterministic_replay")
        self.assertEqual(summary.facts.recovered_terminal_wal_sequence, 7)
        self.assertIs(summary.facts.torn_tail_ignored, False)
        self.assertEqual([attempt.wal_sequence for attempt in summary.cancel_attempts], [5, 6, 7])
        self.assertEqual([attempt.request_id for attempt in summary.cancel_attempts], [2002] * 3)
        self.assertEqual([attempt.recovered_result for attempt in summary.cancel_attempts],
                         [ReplayResult.CANCEL_NOT_OWNER, ReplayResult.CANCELLED, ReplayResult.CANCEL_NOT_FOUND])
        text = summary.summary
        positions = [text.index(f"WAL sequence {sequence}") for sequence in (5, 6, 7)]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("final recovered status is not_resting", text)
        self.assertIn("recovered WAL prefix through sequence 7", text)
        self.assertNotIn("final recovered status is cancelled", text)

    def test_business_rejected_submission_remains_durable_and_not_resting(self):
        evidence, proposal = self.rejected()
        result = self.run_proposal(proposal, evidence)
        self.assertEqual(result.status, RunStatus.ACCEPTED)
        text = result.historical_summary.summary
        self.assertIn("durable submission", text)
        self.assertIn("deterministically replayed as InsufficientFunds", text)
        self.assertIn("Ledger trades total 0", text)
        self.assertIn("final recovered status is not_resting", text)
        self.assertNotIn("before durability", text)
        self.assertEqual(result.historical_summary.cancel_attempts, ())

    def test_wrong_owner_attempt_can_coexist_with_final_resting_state(self):
        evidence = json.loads((FIXTURES / "d2e_historical_wrong_owner.json").read_text())
        _, lifecycle = self.lifecycle()
        proposal = deepcopy(self.proposal)
        proposal["facts"]["recovered_terminal_wal_sequence"] = 5
        proposal["cancel_attempts"] = lifecycle["cancel_attempts"][:1]
        proposal["evidence_refs"].append({"kind": "wal_sequence", "id": 5})
        result = self.run_proposal(proposal, evidence)
        self.assertEqual(result.status, RunStatus.ACCEPTED)
        self.assertIn("CancelNotOwner", result.historical_summary.summary)
        self.assertEqual(result.historical_summary.facts.final_status, "resting")

    def test_missing_unknown_null_and_extra_lifecycle_fields_are_parse_errors(self):
        _, original = self.lifecycle()
        for field in ("submission_recovered_result", "outcome_basis",
                      "recovered_terminal_wal_sequence", "torn_tail_ignored"):
            proposal = deepcopy(original)
            del proposal["facts"][field]
            self.assertTrue(parse_historical_summary_json(json.dumps(proposal)).errors)
        for field, value in (("submission_recovered_result", None),
                             ("submission_recovered_result", "accepted"),
                             ("submission_recovered_result", "Mystery"),
                             ("outcome_basis", "original_response"), ("outcome_basis", None),
                             ("recovered_terminal_wal_sequence", True),
                             ("recovered_terminal_wal_sequence", 1 << 64),
                             ("torn_tail_ignored", 0), ("torn_tail_ignored", None)):
            with self.subTest(field=field, value=value):
                proposal = deepcopy(original)
                proposal["facts"][field] = value
                self.assertTrue(parse_historical_summary_json(json.dumps(proposal)).errors)
        for attempts in (None, {}, [None], original["cancel_attempts"] + [original["cancel_attempts"][0]]):
            proposal = dict(original, cancel_attempts=attempts)
            self.assertTrue(parse_historical_summary_json(json.dumps(proposal)).errors)
        for field, value in (("wal_sequence", None), ("account_id", True),
                             ("request_id", 0), ("recovered_result", "Accepted"),
                             ("recovered_result", "Unknown"), ("client_received", True)):
            proposal = deepcopy(original)
            proposal["cancel_attempts"][0][field] = value
            self.assertTrue(parse_historical_summary_json(json.dumps(proposal)).errors)
        proposal = deepcopy(original)
        del proposal["cancel_attempts"][0]["request_id"]
        self.assertTrue(parse_historical_summary_json(json.dumps(proposal)).errors)

    def test_false_submission_success_and_rejection_are_grounding_errors(self):
        rejected, proposal = self.rejected()
        proposal["facts"]["submission_recovered_result"] = "Accepted"
        result = self.run_proposal(proposal, rejected)
        self.assertEqual(result.status, RunStatus.VALIDATION_REJECTED)
        self.assertEqual(result.errors[0].path, "facts.submission_recovered_result")
        proposal = deepcopy(self.proposal)
        proposal["facts"]["submission_recovered_result"] = "AccountNotFound"
        self.assertEqual(self.run_proposal(proposal).status, RunStatus.VALIDATION_REJECTED)

    def test_cancel_fields_are_grounded_by_exact_wal_entry(self):
        evidence, original = self.lifecycle()
        for index, field, value in ((0, "recovered_result", "Cancelled"),
                                    (1, "recovered_result", "CancelNotFound"),
                                    (2, "recovered_result", "Cancelled"),
                                    (0, "account_id", 11), (0, "request_id", 999),
                                    (2, "wal_sequence", 8)):
            with self.subTest(index=index, field=field):
                proposal = deepcopy(original)
                proposal["cancel_attempts"][index][field] = value
                result = self.run_proposal(proposal, evidence)
                self.assertEqual(result.status, RunStatus.VALIDATION_REJECTED)
                self.assertIsNone(result.historical_summary)

    def test_missing_invented_and_reordered_attempts_are_rejected(self):
        evidence, original = self.lifecycle()
        invented = dict(original["cancel_attempts"][-1], wal_sequence=99)
        for attempts in (original["cancel_attempts"][:-1], [],
                         original["cancel_attempts"] + [invented],
                         original["cancel_attempts"][::-1]):
            proposal = dict(original, cancel_attempts=attempts)
            self.assertEqual(self.run_proposal(proposal, evidence).status, RunStatus.VALIDATION_REJECTED)

    def test_every_lifecycle_and_trade_claim_requires_its_own_reference(self):
        evidence, original = self.lifecycle()
        for omitted in original["evidence_refs"]:
            with self.subTest(omitted=omitted):
                proposal = deepcopy(original)
                proposal["evidence_refs"].remove(omitted)
                result = self.run_proposal(proposal, evidence)
                self.assertEqual(result.status, RunStatus.VALIDATION_REJECTED)
                self.assertEqual(result.errors[-1].category, "invalid_evidence_reference")
        proposal = deepcopy(original)
        proposal["evidence_refs"] = [{"kind": "wal_sequence", "id": 2}]
        self.assertEqual(self.run_proposal(proposal, evidence).status, RunStatus.VALIDATION_REJECTED)
        proposal = deepcopy(original)
        proposal["evidence_refs"].append({"kind": "wal_sequence", "id": 3})
        self.assertEqual(self.run_proposal(proposal, evidence).status, RunStatus.VALIDATION_REJECTED)
        # WAL 5 and Ledger 5 are different required references, not duplicates.
        self.assertEqual(self.run_proposal(original, evidence).status, RunStatus.ACCEPTED)

    def test_recovery_boundary_and_torn_tail_are_grounded_exactly(self):
        evidence, original = self.lifecycle()
        for field, value in (("recovered_terminal_wal_sequence", 8), ("torn_tail_ignored", True)):
            proposal = deepcopy(original)
            proposal["facts"][field] = value
            self.assertEqual(self.run_proposal(proposal, evidence).status, RunStatus.VALIDATION_REJECTED)
        evidence["recovery"]["ignored_torn_tail"] = True
        original["facts"]["torn_tail_ignored"] = True
        result = self.run_proposal(original, evidence)
        self.assertEqual(result.status, RunStatus.ACCEPTED)
        self.assertIn("torn tail ignored: yes", result.historical_summary.summary)

    def test_response_delivery_and_causes_never_enter_trusted_lifecycle(self):
        evidence, original = self.lifecycle()
        for field in ("response_delivered", "client_received", "original_response",
                      "pre_execution_book", "matching_stop_reason", "cause"):
            for location in ("root", "facts", "attempt"):
                proposal = deepcopy(original)
                target = (proposal if location == "root" else proposal["facts"]
                          if location == "facts" else proposal["cancel_attempts"][0])
                target[field] = "invented"
                self.assertEqual(self.run_proposal(proposal, evidence).status, RunStatus.PARSE_ERROR)
        original["summary"] = "The client received Accepted. Matching stopped because of liquidity; book was 500."
        result = self.run_proposal(original, evidence)
        self.assertEqual(result.status, RunStatus.ACCEPTED)
        self.assertNotIn("client received", result.historical_summary.summary)
        self.assertNotIn("book was 500", result.historical_summary.summary)
        self.assertIn("book was 500", result.raw_provider_output)

    def test_lifecycle_prompt_describes_complete_grounding_without_extra_tools(self):
        evidence, _ = self.lifecycle()
        prompt = build_historical_summary_prompt(json.dumps(evidence))
        for required in ("submission_recovered_result", "deterministic_replay",
                         "EVERY evidence.cancel_attempts", "RequestId is not a deduplication key",
                         "recovery.through_wal_sequence", "recovery.ignored_torn_tail",
                         "every trades[].ledger_sequence", "own exact WAL reference"):
            self.assertIn(required, prompt)
