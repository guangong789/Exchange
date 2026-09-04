#!/usr/bin/env python3
"""Manual Binance Agentic Wallet onboarding helper.

The baw CLI owns session persistence. This helper only coordinates the
documented signin/verify/status flow and never prints session credentials.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import webbrowser


def run_json(baw: str, *arguments: str, timeout: int) -> dict:
    completed = subprocess.run(
        [baw, *arguments, "--json"],
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )
    try:
        response = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError("baw did not return valid JSON") from error
    if completed.returncode != 0 or response.get("success") is not True:
        error = response.get("error", {})
        name = error.get("name", "UNKNOWN_ERROR")
        message = error.get("message", "baw command failed")
        raise RuntimeError(f"{name}: {message}")
    return response


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Authorize Binance Agentic Wallet for preview-only use"
    )
    parser.add_argument(
        "--baw", default="baw", help="path to the Binance Agentic Wallet CLI"
    )
    parser.add_argument(
        "--no-browser", action="store_true", help="print the URL without opening it"
    )
    args = parser.parse_args()

    try:
        signin = run_json(args.baw, "auth", "signin", timeout=30)
        data = signin.get("data", {})
        if data.get("status") == "ALREADY_CONNECTED":
            print("Binance Agentic Wallet is already connected.")
            return 0

        qr_code_id = data.get("qrCodeId")
        pairing_code = data.get("pairingCode")
        url = data.get("urlForWeb")
        if not all(isinstance(value, str) and value for value in (
            qr_code_id,
            pairing_code,
            url,
        )):
            raise RuntimeError("baw signin response is missing onboarding fields")

        print(f"Pairing code: {pairing_code}")
        print(f"Open this Binance authorization URL: {url}")
        if not args.no_browser:
            webbrowser.open(url)
        print("Confirm the matching pairing code in Binance App; waiting for verification...")

        run_json(
            args.baw,
            "auth",
            "verify",
            "--qrCodeId",
            qr_code_id,
            timeout=330,
        )
        status = run_json(args.baw, "wallet", "status", timeout=30)
        if status.get("data", {}).get("status") != "CONNECTED":
            raise RuntimeError("verification finished but wallet is not CONNECTED")
        print("Binance Agentic Wallet is connected. Session is managed by baw.")
        return 0
    except (OSError, subprocess.TimeoutExpired, RuntimeError) as error:
        print(f"Wallet login failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
