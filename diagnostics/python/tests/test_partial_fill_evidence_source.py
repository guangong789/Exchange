from contextlib import redirect_stderr, redirect_stdout
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from diagnostics.python.partial_fill_evidence_source import (
    MAX_EVIDENCE_BYTES, load_partial_fill_evidence_from_executable,
)
from diagnostics.python.partial_fill_tool import get_partial_fill_evidence
from diagnostics.python.run_tool_diagnosis import main
from diagnostics.python.tool_diagnostic_runner import (
    FakeToolDiagnosticProvider, ToolRunStatus, run_tool_diagnosis,
)
from diagnostics.python.diagnostic_runner import RunStatus


FIXTURES = Path(__file__).parent / "fixtures"
QUESTION = "Why was order 3 only partially filled?"


class ExecutableEvidenceSourceTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.directory = Path(directory.name)
        # A name containing spaces also checks that the path remains one argv item.
        self.executable = self.directory / "fake evidence producer"
        environment = patch.dict(os.environ, {
            "EXCHANGE_DIAGNOSTIC_EVIDENCE_BIN": str(self.executable),
            "EXCHANGE_EVIDENCE_TIMEOUT_SECONDS": "3",
        }, clear=True)
        environment.start()
        self.addCleanup(environment.stop)
        self.evidence = json.loads((FIXTURES / "d0_partial_fill.json").read_text())
        self.valid = (FIXTURES / "valid_partial_fill_diagnosis.json").read_text()
        self.selection = {"role": "assistant", "content": None, "tool_calls": [{
            "id": "call_fixture", "type": "function", "function": {
                "name": "get_partial_fill_evidence", "arguments": '{"order_id":3}',
            },
        }]}
        self.processes = []
        real_popen = subprocess.Popen

        def launch(*args, **kwargs):
            process = real_popen(*args, **kwargs)
            self.processes.append(process)
            return process

        # These are real, temporary local processes; never the C++ binary or API.
        popen = patch("diagnostics.python.evidence_process.subprocess.Popen", side_effect=launch)
        self.popen = popen.start()
        self.addCleanup(popen.stop)
        self.addCleanup(self.cleanup_processes)
        self.emit(json.dumps(self.evidence).encode())

    def cleanup_processes(self):
        for process in self.processes:
            if process.poll() is None:
                process.kill()
            process.wait()
            if process.stdout is not None:
                process.stdout.close()

    def script(self, body):
        self.executable.write_text(f"#!{sys.executable}\n" + body, encoding="utf-8")
        self.executable.chmod(0o700)

    def emit(self, body):
        self.script(f"import sys\nsys.stdout.buffer.write({body!r})\n")

    def assert_unavailable(self, result, reason=None):
        self.assertEqual(result["error"], "evidence_unavailable")
        self.assertEqual(result["order_id"], 3)
        if reason is not None:
            self.assertEqual(result["reason"], reason)
        self.assertNotIn(str(self.directory), json.dumps(result))

    def test_valid_output_returns_evidence_and_reaps_child(self):
        self.assertEqual(load_partial_fill_evidence_from_executable(3), self.evidence)
        self.popen.assert_called_once()
        args, kwargs = self.popen.call_args
        self.assertEqual(args[0], [str(self.executable)])
        self.assertIs(kwargs["shell"], False)
        self.assertEqual(kwargs["stderr"], subprocess.DEVNULL)
        self.assertEqual(kwargs["stdin"], subprocess.DEVNULL)
        self.assertEqual(self.processes[0].returncode, 0)
        self.assertTrue(self.processes[0].stdout.closed)

    def test_unconfigured_binary_never_launches_or_falls_back(self):
        os.environ.pop("EXCHANGE_DIAGNOSTIC_EVIDENCE_BIN")
        result = get_partial_fill_evidence(3, evidence_source="executable")
        self.assert_unavailable(result, "executable_not_configured")
        self.popen.assert_not_called()

    def test_missing_executable_maps_to_tool_execution_error(self):
        self.executable.unlink()
        result = run_tool_diagnosis(
            QUESTION, FakeToolDiagnosticProvider(self.selection, self.valid),
            evidence_source="executable",
        )
        self.assertEqual(result.status, ToolRunStatus.TOOL_EXECUTION_ERROR)
        self.assert_unavailable(result.tool_error, "process_unavailable")
        self.assertIsNone(result.diagnosis_run)
        self.popen.assert_called_once()

    def test_nonzero_exit_rejects_even_valid_stdout_and_hides_stderr(self):
        body = json.dumps(self.evidence)
        self.script(f"import sys\nsys.stdout.write({body!r})\nsys.stderr.write('private-debug-detail')\nsys.exit(7)\n")
        result = load_partial_fill_evidence_from_executable(3)
        self.assert_unavailable(result, "nonzero_exit")
        self.assertNotIn("private-debug-detail", json.dumps(result))
        self.assertEqual(self.processes[0].returncode, 7)
        self.assertTrue(self.processes[0].stdout.closed)

    def test_timeout_kills_and_reaps_without_retry(self):
        self.script("import time\ntime.sleep(60)\n")
        os.environ["EXCHANGE_EVIDENCE_TIMEOUT_SECONDS"] = "0.15"
        result = load_partial_fill_evidence_from_executable(3)
        self.assert_unavailable(result, "process_timeout")
        self.popen.assert_called_once()
        self.assertLess(self.processes[0].returncode, 0)
        self.assertTrue(self.processes[0].stdout.closed)

    def test_timeout_also_covers_process_that_closes_stdout_then_hangs(self):
        self.script("import os, time\nos.close(1)\ntime.sleep(60)\n")
        os.environ["EXCHANGE_EVIDENCE_TIMEOUT_SECONDS"] = "0.15"
        self.assert_unavailable(load_partial_fill_evidence_from_executable(3), "process_timeout")
        self.assertLess(self.processes[0].returncode, 0)
        self.assertTrue(self.processes[0].stdout.closed)

    def test_invalid_json_or_utf8_output_is_rejected(self):
        for body in (b"{", b"\xff", b'{"case_type":"partial_fill","case_type":"partial_fill"}'):
            with self.subTest(body=body):
                self.emit(body)
                self.assert_unavailable(load_partial_fill_evidence_from_executable(3))

    def test_wrong_case_type_or_target_id_is_rejected(self):
        evidence = dict(self.evidence, case_type="other")
        self.emit(json.dumps(evidence).encode())
        self.assert_unavailable(load_partial_fill_evidence_from_executable(3))
        evidence = dict(self.evidence, target_order={"order_id": 4})
        self.emit(json.dumps(evidence).encode())
        self.assertEqual(load_partial_fill_evidence_from_executable(3), {
            "error": "order_not_found", "order_id": 3,
        })

    def test_contract_requires_sections_used_by_d1(self):
        for section in ("target_order", "pre_execution_book", "execution", "post_execution",
                        "provenance", "trades", "matched_quantity"):
            with self.subTest(section=section):
                evidence = dict(self.evidence)
                del evidence[section]
                self.emit(json.dumps(evidence).encode())
                self.assert_unavailable(load_partial_fill_evidence_from_executable(3))

    def test_contract_rejects_wrong_section_types_and_boolean_identity(self):
        for section, value in (("target_order", []), ("trades", {}), ("matched_quantity", True),
                               ("target_order", {"order_id": True})):
            with self.subTest(section=section, value=value):
                self.emit(json.dumps(dict(self.evidence, **{section: value})).encode())
                self.assert_unavailable(load_partial_fill_evidence_from_executable(3))

    def test_oversized_stdout_stops_and_reaps_a_continuously_writing_child(self):
        self.script("import os\nwhile True:\n    os.write(1, b'x' * 8192)\n")
        self.assert_unavailable(load_partial_fill_evidence_from_executable(3), "output_too_large")
        self.popen.assert_called_once()
        self.assertLess(self.processes[0].returncode, 0)
        self.assertTrue(self.processes[0].stdout.closed)

    def test_exact_output_bound_is_allowed(self):
        body = json.dumps(self.evidence).encode()
        self.emit(body)
        self.assertEqual(load_partial_fill_evidence_from_executable(3, max_output_bytes=len(body)), self.evidence)
        self.assert_unavailable(load_partial_fill_evidence_from_executable(3, max_output_bytes=len(body) - 1),
                                "output_too_large")
        self.assertLess(len(body), MAX_EVIDENCE_BYTES)

    def test_invalid_configuration_does_not_start_a_process(self):
        for value in ("0", "-1", "nan", "inf", "not-a-number"):
            with self.subTest(value=value):
                os.environ["EXCHANGE_EVIDENCE_TIMEOUT_SECONDS"] = value
                self.assert_unavailable(load_partial_fill_evidence_from_executable(3), "invalid_configuration")
        self.popen.assert_not_called()

    def test_unknown_order_does_not_receive_the_fixed_case(self):
        self.assertEqual(load_partial_fill_evidence_from_executable(999), {
            "error": "order_not_found", "order_id": 999,
        })

    def test_executable_path_does_not_read_the_fixture(self):
        result = get_partial_fill_evidence(
            3, evidence_source="executable", evidence_path=self.directory / "missing-fixture.json",
        )
        self.assertEqual(result, self.evidence)

    def test_fixture_source_remains_explicit_and_does_not_launch(self):
        self.assertEqual(get_partial_fill_evidence(3, evidence_source="fixture"), self.evidence)
        self.popen.assert_not_called()

    def test_fake_provider_with_executable_source_still_uses_d1_grounding(self):
        result = run_tool_diagnosis(
            QUESTION, FakeToolDiagnosticProvider(self.selection, self.valid),
            evidence_source="executable",
        )
        self.assertEqual(result.status, RunStatus.ACCEPTED)
        proposal = json.loads(self.valid)
        proposal["facts"]["matched_quantity"] = 4
        rejected = run_tool_diagnosis(
            QUESTION, FakeToolDiagnosticProvider(self.selection, json.dumps(proposal)),
            evidence_source="executable",
        )
        self.assertEqual(rejected.status, RunStatus.VALIDATION_REJECTED)
        self.assertEqual(rejected.to_dict()["errors"][0]["category"], "contradictory_fact")

    def test_source_failure_prevents_final_provider_call(self):
        self.emit(b"{")
        with patch.object(FakeToolDiagnosticProvider, "complete_with_evidence_tool") as final:
            result = run_tool_diagnosis(
                QUESTION, FakeToolDiagnosticProvider(self.selection, self.valid),
                evidence_source="executable",
            )
        self.assertEqual(result.status, ToolRunStatus.TOOL_EXECUTION_ERROR)
        final.assert_not_called()

    def test_cli_can_explicitly_choose_executable_source(self):
        selection_file = self.directory / "selection.json"
        response_file = self.directory / "response.json"
        selection_file.write_text(json.dumps(self.selection))
        response_file.write_text(self.valid)
        output = io.StringIO()
        with patch("sys.argv", ["run_tool_diagnosis", QUESTION, "--evidence-source", "executable",
                                "--selection-file", str(selection_file), "--response-file", str(response_file)]), \
                redirect_stdout(output):
            code = main()
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(output.getvalue())["status"], "accepted")
        self.popen.assert_called_once()

    def test_cli_requires_explicit_source(self):
        with patch("sys.argv", ["run_tool_diagnosis", QUESTION, "--provider", "deepseek"]), \
                redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
            main()
        self.assertEqual(raised.exception.code, 2)
        self.popen.assert_not_called()


if __name__ == "__main__":
    unittest.main()
