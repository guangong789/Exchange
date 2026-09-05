import json
import subprocess
import sys
import unittest
from unittest.mock import patch
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import server


class BridgeTest(unittest.TestCase):
    @patch("server.demo_binary")
    @patch("server.subprocess.run")
    def test_returns_runtime_presentation_json(self, run, binary):
        binary.return_value.is_file.return_value = True
        run.return_value = subprocess.CompletedProcess([], 0, json.dumps({"scenario": "agent-error"}), "")
        payload, status = server.run_scenario("agent-error")
        self.assertEqual(status, 200)
        self.assertEqual(payload["scenario"], "agent-error")
        self.assertEqual(run.call_args.args[0][-1], "agent-error")

    @patch("server.demo_binary")
    @patch("server.subprocess.run")
    def test_returns_safe_missing_key_error(self, run, binary):
        binary.return_value.is_file.return_value = True
        run.return_value = subprocess.CompletedProcess([], 1, json.dumps({"error": "DEEPSEEK_API_KEY 未设置"}), "sensitive stderr")
        payload, status = server.run_scenario("normal")
        self.assertEqual(status, 503)
        self.assertEqual(payload, {"error": "DEEPSEEK_API_KEY 未设置"})

    @patch("server.simulation_binary")
    @patch("server.subprocess.Popen")
    def test_forwards_simulation_command_without_interpreting_runtime_state(
            self, popen, binary):
        binary.return_value.is_file.return_value = True
        process = popen.return_value
        process.poll.return_value = None
        process.stdout.readline.return_value = json.dumps({
            "ok": True,
            "snapshot": {"status": "RUNNING", "round": {"current": 1}},
        }) + "\n"

        payload, status = server.SimulationBridge().command({"action": "advance"})

        self.assertEqual(status, 200)
        self.assertEqual(payload["status"], "RUNNING")
        self.assertEqual(payload["round"]["current"], 1)
        self.assertEqual(process.stdin.write.call_args.args[0], '{"action": "advance"}\n')

    @patch("server.simulation_binary")
    @patch("server.subprocess.Popen")
    def test_forwards_live_x402_evidence_without_wallet_logic(self, popen, binary):
        binary.return_value.is_file.return_value = True
        process = popen.return_value
        process.poll.return_value = None
        process.stdout.readline.return_value = json.dumps({
            "ok": True,
            "live_x402": {"status": "COMPLETE", "provider_status": "ACTION_REQUIRED"},
        }) + "\n"

        payload, status = server.SimulationBridge().command({"action": "live-x402-check"})

        self.assertEqual(status, 200)
        self.assertEqual(payload["provider_status"], "ACTION_REQUIRED")
        self.assertEqual(process.stdin.write.call_args.args[0], '{"action": "live-x402-check"}\n')

    @patch("server.simulation_binary")
    @patch("server.subprocess.Popen")
    def test_returns_bridge_error_without_exposing_child_stderr(self, popen, binary):
        binary.return_value.is_file.return_value = True
        process = popen.return_value
        process.poll.return_value = None
        process.stdout.readline.return_value = json.dumps({
            "ok": False,
            "error": "DEEPSEEK_API_KEY 未设置",
        }) + "\n"

        payload, status = server.SimulationBridge().command({"action": "start", "scenario": "normal"})

        self.assertEqual(status, 503)
        self.assertEqual(payload, {"error": "DEEPSEEK_API_KEY 未设置"})

    def test_agent_error_simulation_smoke_reaches_manual_exact_replay(self):
        bridge = server.SimulationBridge()
        try:
            payload, status = bridge.command({"action": "start", "scenario": "agent-error"})
            self.assertEqual(status, 200)
            self.assertEqual(payload["status"], "RUNNING")
            while payload["status"] == "RUNNING":
                payload, status = bridge.command({"action": "advance"})
                self.assertEqual(status, 200)
            self.assertEqual(payload["status"], "MAX_ROUNDS")
            self.assertEqual(
                payload["summary"]["rejected_actions"],
                payload["round"]["max"],
            )
            self.assertEqual(payload["summary"]["trades"], 0)
            self.assertEqual(payload["summary"]["quote_spent"], 0)
            self.assertEqual(payload["summary"]["current_reserved_quote"], 0)
            self.assertEqual(payload["replay"]["status"], "NOT_RUN")

            payload, status = bridge.command({"action": "replay-start"})
            self.assertEqual(status, 200)
            while payload["replay"]["status"] == "RUNNING":
                payload, status = bridge.command({"action": "replay-advance"})
                self.assertEqual(status, 200)
            self.assertEqual(payload["replay"]["status"], "EXACT")
            self.assertEqual(payload["replay"]["deepseek_calls_replay"], 0)
            self.assertEqual(payload["replay"]["payment_service_calls_replay"], 0)
            self.assertEqual(len(payload["replay"]["captured_economic_inputs"]),
                             payload["round"]["max"])
            self.assertEqual(payload["replay"]["original_final_state"],
                             payload["replay"]["replay_final_state"])
        finally:
            bridge.close()


if __name__ == "__main__":
    unittest.main()
