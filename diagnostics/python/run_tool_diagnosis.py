"""One-call, two-tool CLI; only --provider deepseek enables live requests."""

import argparse
import json
from pathlib import Path

from .deepseek_diagnostic_provider import DeepSeekDiagnosticProvider
from .diagnostic_runner import RunStatus
from .partial_fill_diagnosis import _load_json
from .tool_diagnostic_runner import (
    FakeToolDiagnosticProvider, ToolDiagnosticRunResult, ToolRunStatus, run_tool_diagnosis,
)


def main() -> int:
    parser = argparse.ArgumentParser(description="Diagnose partial fill or summarize historical durable facts")
    parser.add_argument("question")
    parser.add_argument("--provider", choices=("fake", "deepseek"), default="fake")
    parser.add_argument("--evidence-source", choices=("fixture", "executable"), required=True)
    parser.add_argument("--selection-file", type=Path, help="Fake raw assistant/tool-call message JSON")
    parser.add_argument("--response-file", type=Path, help="Fake final diagnosis text")
    args = parser.parse_args()
    if args.provider == "fake" and (args.selection_file is None or args.response_file is None):
        parser.error("fake provider requires --selection-file and --response-file")
    if args.provider == "deepseek" and (args.selection_file is not None or args.response_file is not None):
        parser.error("deepseek provider cannot use fake response files")
    try:
        if args.provider == "fake":
            provider = FakeToolDiagnosticProvider(
                _load_json(args.selection_file.read_text(encoding="utf-8")),
                args.response_file.read_text(encoding="utf-8"),
            )
        else:
            provider = DeepSeekDiagnosticProvider()
    except (OSError, UnicodeError, ValueError, TypeError):
        result = ToolDiagnosticRunResult(
            ToolRunStatus.TOOL_CALL_INVALID, tool_error={"error": "invalid_fake_input"},
        )
    else:
        result = run_tool_diagnosis(args.question, provider, evidence_source=args.evidence_source)
    print(json.dumps(result.to_dict(), indent=2))
    return 0 if result.status == RunStatus.ACCEPTED else 1


if __name__ == "__main__":
    raise SystemExit(main())
