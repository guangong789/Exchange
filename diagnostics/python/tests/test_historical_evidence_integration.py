"""Opt-in checks against the real C++ binary and the offline D2-C workload.

Set EXCHANGE_RUN_HISTORICAL_INTEGRATION=1 plus the three historical source
variables. Inputs must be the D2-C fixture (orders 1/2/3), after its writer exits.
The standard Python suite neither creates a WAL nor requires a C++ build.

EXCHANGE_RUN_HISTORICAL_LIFECYCLE_INTEGRATION=1 selects the lifecycle workload:
the same initial four commands, three cancels of order 2 (wrong owner, owner,
repeated owner), then an insufficient-funds submit creating order 4 at WAL 8.
"""

import json
import os
from pathlib import Path
import unittest

from diagnostics.python.diagnostic_runner import RunStatus
from diagnostics.python.historical_order_tool import get_historical_order_evidence
from diagnostics.python.tool_diagnostic_runner import FakeToolDiagnosticProvider, run_tool_diagnosis


@unittest.skipUnless(os.environ.get("EXCHANGE_RUN_HISTORICAL_INTEGRATION") == "1",
                     "Requires explicit offline D2-C C++ integration inputs")
class HistoricalEvidenceIntegrationTest(unittest.TestCase):
    def test_real_resting_order_and_grounded_summary(self):
        evidence = get_historical_order_evidence(2)
        self.assertNotIn("error", evidence)
        self.assertEqual(evidence["submission"]["quantity"], 5)
        self.assertEqual(evidence["matched_quantity"], 3)
        self.assertEqual(evidence["final_state"]["remaining_quantity"], 2)
        self.assertEqual(evidence["final_state"]["status"], "resting")
        selection = {"role": "assistant", "tool_calls": [{
            "id": "integration_call", "type": "function", "function": {
                "name": "get_historical_order_evidence", "arguments": '{"order_id":2}',
            },
        }]}
        valid = (Path(__file__).parent / "fixtures/valid_historical_order_summary.json").read_text()
        result = run_tool_diagnosis("What happened to historical order 2 after recovery?",
                                    FakeToolDiagnosticProvider(selection, valid), evidence_source="executable")
        self.assertEqual(result.status, RunStatus.ACCEPTED)

    def test_real_fully_filled_order(self):
        evidence = get_historical_order_evidence(1)
        self.assertNotIn("error", evidence)
        self.assertEqual(evidence["matched_quantity"], 2)
        self.assertEqual(evidence["final_state"]["remaining_quantity"], 0)
        self.assertEqual(evidence["final_state"]["status"], "fully_filled")


@unittest.skipUnless(os.environ.get("EXCHANGE_RUN_HISTORICAL_LIFECYCLE_INTEGRATION") == "1",
                     "Requires explicit offline D2-E lifecycle C++ integration inputs")
class HistoricalLifecycleIntegrationTest(unittest.TestCase):
    def setUp(self):
        self.wal_path = Path(os.environ["EXCHANGE_HISTORICAL_EVIDENCE_WAL"])
        self.original_wal = self.wal_path.read_bytes()

    def tearDown(self):
        self.assertEqual(self.wal_path.read_bytes(), self.original_wal)

    def grounded_summary(self, order_id, fixture):
        proposal = json.loads((Path(__file__).parent / "fixtures" / fixture).read_text())
        proposal["facts"]["recovered_terminal_wal_sequence"] = 8
        if order_id == 4:
            proposal["evidence_refs"] = [{"kind": "wal_sequence", "id": 8}]
        selection = {"role": "assistant", "tool_calls": [{
            "id": "lifecycle_call", "type": "function", "function": {
                "name": "get_historical_order_evidence",
                "arguments": json.dumps({"order_id": order_id}),
            },
        }]}
        result = run_tool_diagnosis(f"What happened to historical order {order_id} after recovery?",
            FakeToolDiagnosticProvider(selection, json.dumps(proposal)), evidence_source="executable")
        self.assertEqual(result.status, RunStatus.ACCEPTED)
        return result.historical_run.historical_summary

    def test_real_order_lifecycle_and_complete_grounded_summary(self):
        evidence = get_historical_order_evidence(2)
        self.assertNotIn("error", evidence)
        self.assertEqual(evidence["outcome_basis"], "deterministic_replay")
        self.assertEqual(evidence["submission"]["recovered_result"], "Accepted")
        self.assertEqual([attempt["wal_sequence"] for attempt in evidence["cancel_attempts"]], [5, 6, 7])
        self.assertEqual([attempt["request_id"] for attempt in evidence["cancel_attempts"]], [2002] * 3)
        self.assertEqual([attempt["recovered_result"] for attempt in evidence["cancel_attempts"]],
                         ["CancelNotOwner", "Cancelled", "CancelNotFound"])
        self.assertEqual(evidence["matched_quantity"], 3)
        self.assertEqual(evidence["final_state"]["status"], "not_resting")
        self.assertIsNone(evidence["final_state"]["remaining_quantity"])
        self.assertEqual(evidence["recovery"], {"through_wal_sequence": 8, "ignored_torn_tail": False})
        summary = self.grounded_summary(2, "valid_historical_lifecycle_summary.json")
        self.assertEqual([attempt.wal_sequence for attempt in summary.cancel_attempts], [5, 6, 7])
        self.assertEqual(summary.facts.recovered_terminal_wal_sequence, 8)

    def test_real_durable_business_rejection_and_grounded_summary(self):
        evidence = get_historical_order_evidence(4)
        self.assertNotIn("error", evidence)
        self.assertEqual(evidence["submission"]["wal_sequence"], 8)
        self.assertEqual(evidence["submission"]["recovered_result"], "InsufficientFunds")
        self.assertEqual(evidence["cancel_attempts"], [])
        self.assertEqual(evidence["trades"], [])
        self.assertEqual(evidence["matched_quantity"], 0)
        self.assertEqual(evidence["final_state"]["status"], "not_resting")
        summary = self.grounded_summary(4, "valid_historical_rejected_summary.json")
        self.assertEqual(summary.facts.submission_recovered_result, "InsufficientFunds")
        self.assertIn("durable submission", summary.summary)
