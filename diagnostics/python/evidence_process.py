"""Bounded local process boundary shared by the two C++ evidence producers."""

import math
import os
import selectors
import subprocess
import time
from typing import Any


DEFAULT_TIMEOUT_SECONDS = 5.0
MAX_EVIDENCE_BYTES = 64 * 1024


def read_evidence_process(
    arguments: list[str], order_id: int, *, max_output_bytes: int = MAX_EVIDENCE_BYTES,
) -> tuple[int, str] | dict[str, Any]:
    """Read bounded UTF-8 stdout and reap the child, without shell or retries.

    Return the exit code for the caller's explicit evidence contract handling.
    Both C++ producers are Linux apps; selectable pipes enforce a deadline
    during reading, and wait(timeout) covers exit after stdout EOF.
    """
    def unavailable(reason: str) -> dict[str, Any]:
        return {"error": "evidence_unavailable", "order_id": order_id, "reason": reason}

    try:
        timeout = float(os.environ.get(
            "EXCHANGE_EVIDENCE_TIMEOUT_SECONDS", str(DEFAULT_TIMEOUT_SECONDS),
        ))
        if not math.isfinite(timeout) or timeout <= 0:
            raise ValueError()
    except ValueError:
        return unavailable("invalid_configuration")
    if type(max_output_bytes) is not int or max_output_bytes <= 0:
        return unavailable("invalid_configuration")

    process = None
    output = bytearray()
    deadline = time.monotonic() + timeout
    try:
        process = subprocess.Popen(
            arguments, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, shell=False, bufsize=0,
        )
        assert process.stdout is not None
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not selector.select(remaining):
                    raise subprocess.TimeoutExpired(process.args, timeout)
                chunk = os.read(process.stdout.fileno(), min(8192, max_output_bytes + 1 - len(output)))
                if not chunk:
                    break
                output.extend(chunk)
                if len(output) > max_output_bytes:
                    return unavailable("output_too_large")
        process.wait(timeout=max(0.0, deadline - time.monotonic()))
    except subprocess.TimeoutExpired:
        return unavailable("process_timeout")
    except OSError:
        return unavailable("process_unavailable")
    finally:
        if process is not None:
            if process.poll() is None:
                process.kill()
            process.wait()
            if process.stdout is not None:
                process.stdout.close()
    try:
        raw = output.decode("utf-8")
    except UnicodeError:
        return unavailable("invalid_encoding")
    return process.returncode, raw
