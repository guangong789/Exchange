"""Local diagnostics CLI; real network access requires --provider deepseek."""

import argparse
import json
from pathlib import Path

from .deepseek_diagnostic_provider import DeepSeekDiagnosticProvider
from .diagnostic_provider import FakeDiagnosticProvider, ProviderError, ProviderTimeoutError
from .diagnostic_runner import DiagnosticRunResult, RunStatus, run_diagnosis
from .partial_fill_diagnosis import ErrorCategory, ValidationError


def main() -> int:
    parser = argparse.ArgumentParser(description="Run a partial-fill diagnosis")
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--provider", choices=("fake", "deepseek"), default="fake")
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--response-file", type=Path, help="Fake provider's raw response text")
    source.add_argument("--failure", choices=("timeout", "error"))
    args = parser.parse_args()
    if args.provider == "fake" and args.response_file is None and args.failure is None:
        parser.error("fake provider requires --response-file or --failure")
    if args.provider == "deepseek" and (args.response_file is not None or args.failure is not None):
        parser.error("deepseek provider cannot use fake response/failure options")
    try:
        evidence_json = args.evidence.read_text(encoding="utf-8")
        if args.provider == "deepseek":
            provider = DeepSeekDiagnosticProvider()
        elif args.response_file is not None:
            provider = FakeDiagnosticProvider(args.response_file.read_text(encoding="utf-8"))
        else:
            failure = ProviderTimeoutError() if args.failure == "timeout" else ProviderError()
            provider = FakeDiagnosticProvider(failure=failure)
    except (OSError, UnicodeError) as error:
        # A local file failure is not a provider transport error.
        result = DiagnosticRunResult(
            RunStatus.VALIDATION_REJECTED,
            errors=(ValidationError(ErrorCategory.SCHEMA_ERROR, "input", str(error)),),
        )
    else:
        result = run_diagnosis(evidence_json, provider)
    print(json.dumps(result.to_dict(), indent=2))
    return 0 if result.status == RunStatus.ACCEPTED else 1


if __name__ == "__main__":
    raise SystemExit(main())
