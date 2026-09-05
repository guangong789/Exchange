#!/usr/bin/env python3
"""Local-only presentation bridge for the hackathon demo.

It invokes the existing C++ presentation entry point; it owns no scenario,
matching, payment, or accounting logic.
"""

from __future__ import annotations

import argparse
import atexit
import json
import os
import subprocess
import threading
from http import HTTPStatus
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse


UI_DIRECTORY = Path(__file__).resolve().parent
REPOSITORY_DIRECTORY = UI_DIRECTORY.parent


def demo_binary() -> Path:
    configured = os.environ.get("EXCHANGE_HACKATHON_DEMO_BIN")
    return Path(configured) if configured else REPOSITORY_DIRECTORY / "build" / "exchange_hackathon_demo"


def simulation_binary() -> Path:
    configured = os.environ.get("EXCHANGE_HACKATHON_UI_BRIDGE_BIN")
    return Path(configured) if configured else REPOSITORY_DIRECTORY / "build" / "exchange_hackathon_ui_bridge"


def run_scenario(mode: str) -> tuple[dict, int]:
    """Run one existing C++ scenario and return only its presentation JSON."""
    if mode not in {"normal", "agent-error"}:
        return {"error": "unsupported scenario"}, HTTPStatus.BAD_REQUEST

    binary = demo_binary()
    if not binary.is_file():
        return {"error": "exchange_hackathon_demo is not built"}, HTTPStatus.SERVICE_UNAVAILABLE

    completed = subprocess.run(
        [str(binary), "--json", "--scenario", mode],
        cwd=REPOSITORY_DIRECTORY,
        capture_output=True,
        text=True,
        check=False,
    )
    try:
        payload = json.loads(completed.stdout)
    except json.JSONDecodeError:
        return {"error": "demo runtime returned invalid presentation JSON"}, HTTPStatus.INTERNAL_SERVER_ERROR

    if completed.returncode != 0 or "error" in payload:
        # Deliberately do not expose stderr: it may carry provider details.
        return {"error": payload.get("error", "demo runtime failed")}, HTTPStatus.SERVICE_UNAVAILABLE
    return payload, HTTPStatus.OK


class SimulationBridge:
    """Owns a localhost C++ simulation process; Python only forwards commands."""

    def __init__(self) -> None:
        self._process: subprocess.Popen[str] | None = None
        self._lock = threading.Lock()

    def command(self, request: dict) -> tuple[dict, int]:
        with self._lock:
            binary = simulation_binary()
            if not binary.is_file():
                return {"error": "exchange_hackathon_ui_bridge is not built"}, HTTPStatus.SERVICE_UNAVAILABLE
            if self._process is None or self._process.poll() is not None:
                self._process = subprocess.Popen(
                    [str(binary)], cwd=REPOSITORY_DIRECTORY, text=True,
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    bufsize=1,
                )
            assert self._process.stdin is not None
            assert self._process.stdout is not None
            self._process.stdin.write(json.dumps(request) + "\n")
            self._process.stdin.flush()
            response_line = self._process.stdout.readline()
            try:
                response = json.loads(response_line)
            except json.JSONDecodeError:
                return {"error": "simulation runtime returned invalid JSON"}, HTTPStatus.INTERNAL_SERVER_ERROR
            if not response.get("ok"):
                return {"error": response.get("error", "simulation runtime failed")}, HTTPStatus.SERVICE_UNAVAILABLE
            if "snapshot" in response:
                return response["snapshot"], HTTPStatus.OK
            if "live_x402" in response:
                return response["live_x402"], HTTPStatus.OK
            return {"error": "simulation runtime returned incomplete JSON"}, HTTPStatus.INTERNAL_SERVER_ERROR

    def close(self) -> None:
        with self._lock:
            if self._process is None or self._process.poll() is not None:
                return
            process = self._process
            try:
                assert process.stdin is not None
                process.stdin.write('{"action":"shutdown"}\n')
                process.stdin.flush()
                process.wait(timeout=1)
            except (OSError, subprocess.TimeoutExpired):
                process.terminate()
                process.wait(timeout=1)
            finally:
                for stream in (process.stdin, process.stdout, process.stderr):
                    if stream is not None:
                        stream.close()
                self._process = None


SIMULATION_BRIDGE = SimulationBridge()
atexit.register(SIMULATION_BRIDGE.close)


def run_simulation_command(request: dict) -> tuple[dict, int]:
    return SIMULATION_BRIDGE.command(request)


class DemoRequestHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=UI_DIRECTORY, **kwargs)

    def do_GET(self) -> None:  # noqa: N802 - stdlib hook name
        parsed = urlparse(self.path)
        if parsed.path == "/api/simulation":
            payload, status = run_simulation_command({"action": "snapshot"})
            self._send_json(payload, status)
            return
        if parsed.path == "/api/live-x402":
            payload, status = run_simulation_command({"action": "live-x402-snapshot"})
            self._send_json(payload, status)
            return
        if parsed.path != "/api/scenario":
            super().do_GET()
            return

        mode = parse_qs(parsed.query).get("mode", ["normal"])[0]
        payload, status = run_scenario(mode)
        self._send_json(payload, status)

    def do_POST(self) -> None:  # noqa: N802 - stdlib hook name
        path = urlparse(self.path).path
        if path == "/api/live-x402":
            try:
                size = int(self.headers.get("Content-Length", "0"))
                request = json.loads(self.rfile.read(size))
                if not isinstance(request, dict) or request.get("action") != "run":
                    raise ValueError
            except (ValueError, json.JSONDecodeError):
                self._send_json({"error": "invalid live x402 command"}, HTTPStatus.BAD_REQUEST)
                return
            payload, status = run_simulation_command({"action": "live-x402-check"})
            self._send_json(payload, status)
            return
        if path != "/api/simulation":
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        try:
            size = int(self.headers.get("Content-Length", "0"))
            request = json.loads(self.rfile.read(size))
            if not isinstance(request, dict) or request.get("action") not in {
                "start", "advance", "stop", "replay-start", "replay-advance",
            }:
                raise ValueError
        except (ValueError, json.JSONDecodeError):
            self._send_json({"error": "invalid simulation command"}, HTTPStatus.BAD_REQUEST)
            return
        payload, status = run_simulation_command(request)
        self._send_json(payload, status)

    def _send_json(self, payload: dict, status: int) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)


def main() -> None:
    parser = argparse.ArgumentParser(description="localhost-only Exchange hackathon demo UI")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", args.port), DemoRequestHandler)
    print(f"Serving demo UI at http://127.0.0.1:{server.server_port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
