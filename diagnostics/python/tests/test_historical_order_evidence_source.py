import json
from copy import deepcopy
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from diagnostics.python.historical_order_evidence_source import (
    load_historical_order_evidence_from_executable, parse_historical_order_evidence,
)
from diagnostics.python.historical_order_tool import get_historical_order_evidence


FIXTURES = Path(__file__).parent / "fixtures"


class HistoricalEvidenceSourceTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.executable = self.directory / "historical producer with spaces"
        self.wal = self.directory / "offline input.wal"
        self.config = self.directory / "bootstrap config.json"
        environment = patch.dict(os.environ, {
            "EXCHANGE_HISTORICAL_EVIDENCE_BIN": str(self.executable),
            "EXCHANGE_HISTORICAL_EVIDENCE_WAL": str(self.wal),
            "EXCHANGE_HISTORICAL_EVIDENCE_CONFIG": str(self.config),
            "EXCHANGE_EVIDENCE_TIMEOUT_SECONDS": "3",
        }, clear=True)
        environment.start()
        self.addCleanup(environment.stop)
        self.evidence = json.loads((FIXTURES / "d2c_historical_resting.json").read_text())
        self.processes = []
        original_popen = subprocess.Popen

        def launch(*args, **kwargs):
            process = original_popen(*args, **kwargs)
            self.processes.append(process)
            return process

        popen = patch("diagnostics.python.evidence_process.subprocess.Popen", side_effect=launch)
        self.popen = popen.start()
        self.addCleanup(popen.stop)
        self.emit(json.dumps(self.evidence).encode())

    def script(self, body):
        self.executable.write_text(f"#!{sys.executable}\n" + body)
        self.executable.chmod(0o700)

    def emit(self, body, exit_code=0):
        self.script(f"import sys\nsys.stdout.buffer.write({body!r})\nsys.exit({exit_code})\n")

    def load(self, **kwargs):
        return load_historical_order_evidence_from_executable(2, **kwargs)

    def assert_unavailable(self, result, reason=None):
        self.assertEqual(result["error"], "evidence_unavailable")
        self.assertEqual(result["order_id"], 2)
        if reason:
            self.assertEqual(result["reason"], reason)
        self.assertNotIn(str(self.directory), json.dumps(result))

    def test_success_uses_exact_arguments_no_shell_and_reaps_child(self):
        self.assertEqual(self.load(), self.evidence)
        args, kwargs = self.popen.call_args
        self.assertEqual(args[0], [str(self.executable), "--wal", str(self.wal),
                                  "--order-id", "2", "--config", str(self.config)])
        self.assertIs(kwargs["shell"], False)
        self.assertEqual(kwargs["stderr"], subprocess.DEVNULL)
        self.assertEqual(self.processes[0].returncode, 0)
        self.assertTrue(self.processes[0].stdout.closed)

    def test_all_three_explicit_settings_are_required_without_fallback(self):
        for name in ("EXCHANGE_HISTORICAL_EVIDENCE_BIN", "EXCHANGE_HISTORICAL_EVIDENCE_WAL",
                     "EXCHANGE_HISTORICAL_EVIDENCE_CONFIG"):
            with self.subTest(name=name), patch.dict(os.environ, {name: ""}):
                self.assert_unavailable(get_historical_order_evidence(2), "historical_source_not_configured")
        self.popen.assert_not_called()

    def test_missing_executable(self):
        self.executable.unlink()
        self.assert_unavailable(self.load(), "process_unavailable")
        self.popen.assert_called_once()

    def test_config_wal_or_active_writer_failure_remains_sanitized_nonzero(self):
        self.script("import sys\nsys.stderr.write('private WAL path and lock details')\nsys.exit(1)\n")
        result = self.load()
        self.assert_unavailable(result, "nonzero_exit")
        self.assertNotIn("private", json.dumps(result))
        self.popen.assert_called_once()

    def test_nonzero_rejects_even_valid_evidence(self):
        self.emit(json.dumps(self.evidence).encode(), 7)
        self.assert_unavailable(self.load(), "nonzero_exit")

    def test_not_found_requires_exact_exit_code_identity_and_error_shape(self):
        error = {"error": "order_not_found", "order_id": 2}
        self.emit(json.dumps(error).encode(), 2)
        self.assertEqual(self.load(), error)
        for body, code in ((dict(error, order_id=3), 2), (dict(error, path="secret"), 2),
                           (error, 1), (error, 0), (self.evidence, 2)):
            with self.subTest(body=body, code=code):
                self.emit(json.dumps(body).encode(), code)
                self.assert_unavailable(self.load())

    def test_malformed_json_duplicates_and_encoding(self):
        for body in (b"{", b"\xff", b'{"case_type":"x","case_type":"y"}', b'{"x":NaN}'):
            with self.subTest(body=body):
                self.emit(body)
                self.assert_unavailable(self.load())

    def test_wrong_case_or_order_is_rejected(self):
        self.emit(json.dumps(dict(self.evidence, case_type="partial_fill")).encode())
        self.assert_unavailable(self.load())
        self.emit(json.dumps(dict(self.evidence, order_id=3)).encode())
        self.assertEqual(self.load(), {"error": "order_not_found", "order_id": 2})

    def test_required_contract_sections_and_types(self):
        for section in ("submission", "trades", "matched_quantity", "final_state",
                        "evidence_scope", "order_id", "recovery", "unavailable_context"):
            with self.subTest(section=section):
                evidence = dict(self.evidence)
                del evidence[section]
                self.assert_unavailable(parse_historical_order_evidence(json.dumps(evidence), 2))
        for field, value in (("submission", []), ("trades", {}), ("matched_quantity", True),
                             ("order_id", True), ("evidence_scope", ["execution_time"])):
            with self.subTest(field=field):
                self.assert_unavailable(parse_historical_order_evidence(
                    json.dumps(dict(self.evidence, **{field: value})), 2))

    def test_enriched_fixtures_accept_replay_results_and_all_cancel_attempts(self):
        for filename in ("d2c_historical_resting.json", "d2c_historical_filled.json",
                         "d2e_historical_wrong_owner.json", "d2e_historical_cancelled.json",
                         "d2e_historical_rejected.json"):
            with self.subTest(filename=filename):
                evidence = json.loads((FIXTURES / filename).read_text())
                self.assertEqual(parse_historical_order_evidence(
                    json.dumps(evidence), evidence["order_id"]), evidence)
        lifecycle = json.loads((FIXTURES / "d2e_historical_cancelled.json").read_text())
        self.assertEqual([attempt["request_id"] for attempt in lifecycle["cancel_attempts"]],
                         [lifecycle["submission"]["request_id"]] * 3)

    def test_lifecycle_metadata_is_required_and_closed(self):
        for field in ("outcome_basis", "cancel_attempts"):
            evidence = deepcopy(self.evidence)
            del evidence[field]
            self.assert_unavailable(parse_historical_order_evidence(json.dumps(evidence), 2))
        for basis in (None, "original_response", "persisted_response", "client_observed", True):
            evidence = dict(self.evidence, outcome_basis=basis)
            self.assert_unavailable(parse_historical_order_evidence(json.dumps(evidence), 2))
        for result in (None, "accepted", "Unknown", "Cancelled", "InvalidRequest"):
            evidence = deepcopy(self.evidence)
            evidence["submission"]["recovered_result"] = result
            self.assert_unavailable(parse_historical_order_evidence(json.dumps(evidence), 2))
        evidence = deepcopy(self.evidence)
        del evidence["submission"]["recovered_result"]
        self.assert_unavailable(parse_historical_order_evidence(json.dumps(evidence), 2))
        for section in (None, "submission", "recovery", "final_state", "unavailable_context"):
            evidence = deepcopy(self.evidence)
            target = evidence if section is None else evidence[section]
            target["original_response"] = "Accepted"
            self.assert_unavailable(parse_historical_order_evidence(json.dumps(evidence), 2))

    def test_cancel_entries_require_identity_known_results_and_wal_order(self):
        original = json.loads((FIXTURES / "d2e_historical_cancelled.json").read_text())
        for field in ("request_id", "account_id", "order_id", "wal_sequence", "recovered_result"):
            evidence = deepcopy(original)
            del evidence["cancel_attempts"][0][field]
            self.assert_unavailable(parse_historical_order_evidence(json.dumps(evidence), 2))
        for field, value in (("request_id", True), ("account_id", None), ("order_id", 999),
                             ("wal_sequence", 8), ("wal_sequence", 2),
                             ("recovered_result", "Accepted"), ("recovered_result", "cancelled"),
                             ("client_received", True)):
            evidence = deepcopy(original)
            evidence["cancel_attempts"][0][field] = value
            self.assert_unavailable(parse_historical_order_evidence(json.dumps(evidence), 2))
        for attempts in (None, {}, [None], original["cancel_attempts"][::-1],
                         original["cancel_attempts"] + [original["cancel_attempts"][-1]]):
            evidence = dict(original, cancel_attempts=attempts)
            self.assert_unavailable(parse_historical_order_evidence(json.dumps(evidence), 2))

    def test_recovery_boundary_types_and_integer_ranges_are_checked(self):
        for field, value in (("through_wal_sequence", None), ("through_wal_sequence", True),
                             ("through_wal_sequence", 1), ("through_wal_sequence", 1 << 64),
                             ("ignored_torn_tail", 0), ("ignored_torn_tail", "false")):
            evidence = deepcopy(self.evidence)
            evidence["recovery"][field] = value
            self.assert_unavailable(parse_historical_order_evidence(json.dumps(evidence), 2))
        for field, value in (("account_id", 1 << 64), ("request_id", None),
                             ("price", 1 << 63), ("quantity", 1.0), ("logical_timestamp", False)):
            evidence = deepcopy(self.evidence)
            evidence["submission"][field] = value
            self.assert_unavailable(parse_historical_order_evidence(json.dumps(evidence), 2))
        evidence = deepcopy(self.evidence)
        evidence["recovery"]["ignored_torn_tail"] = True
        self.assertEqual(parse_historical_order_evidence(json.dumps(evidence), 2), evidence)

    def test_trade_identity_and_unavailable_context_cannot_be_fabricated(self):
        for field, value in (("buy_order_id", 999), ("ledger_sequence", True),
                             ("price", None), ("quantity", 1 << 63), ("client_received", True)):
            evidence = deepcopy(self.evidence)
            evidence["trades"][0][field] = value
            self.assert_unavailable(parse_historical_order_evidence(json.dumps(evidence), 2))
        for field in self.evidence["unavailable_context"]:
            evidence = deepcopy(self.evidence)
            evidence["unavailable_context"][field] = "invented"
            self.assert_unavailable(parse_historical_order_evidence(json.dumps(evidence), 2))

    def test_timeout_including_stdout_eof_kills_and_reaps_without_retry(self):
        os.environ["EXCHANGE_EVIDENCE_TIMEOUT_SECONDS"] = "0.15"
        for body in ("import time\ntime.sleep(60)\n",
                     "import os,time\nos.close(1)\ntime.sleep(60)\n"):
            with self.subTest(body=body):
                self.script(body)
                self.popen.reset_mock()
                self.assert_unavailable(self.load(), "process_timeout")
                self.popen.assert_called_once()
                self.assertLess(self.processes[-1].returncode, 0)
                self.assertTrue(self.processes[-1].stdout.closed)

    def test_output_bound_stops_continuous_producer(self):
        self.script("import os\nwhile True:\n    os.write(1, b'x' * 8192)\n")
        self.assert_unavailable(self.load(), "output_too_large")
        self.assertLess(self.processes[-1].returncode, 0)
        self.assertTrue(self.processes[-1].stdout.closed)

    def test_exact_limit_and_invalid_configuration(self):
        body = json.dumps(self.evidence).encode()
        self.assertEqual(self.load(max_output_bytes=len(body)), self.evidence)
        self.assert_unavailable(self.load(max_output_bytes=len(body) - 1), "output_too_large")
        self.popen.reset_mock()
        for timeout in ("0", "nan", "inf", "bad"):
            with patch.dict(os.environ, {"EXCHANGE_EVIDENCE_TIMEOUT_SECONDS": timeout}):
                self.assert_unavailable(self.load(), "invalid_configuration")
        self.popen.assert_not_called()

    def test_fixture_and_executable_are_explicit_independent_sources(self):
        self.assertEqual(get_historical_order_evidence(2, evidence_source="fixture"), self.evidence)
        self.popen.assert_not_called()
        self.assertEqual(get_historical_order_evidence(
            2, evidence_path=self.directory / "missing-fixture.json"), self.evidence)
        self.assert_unavailable(get_historical_order_evidence(2, evidence_source="other"), "invalid_source")

    def test_invalid_order_ids_never_launch(self):
        for value in (True, 2.0, "2", 0, -1):
            self.assertEqual(get_historical_order_evidence(value), {"error": "invalid_order_id"})
        self.popen.assert_not_called()
