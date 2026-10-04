"""Run locally: python3 -m diagnostics.python.validate_diagnosis evidence.json diagnosis.json."""

import argparse
import json
from pathlib import Path

from .partial_fill_diagnosis import (
    ErrorCategory, ValidationError, ValidationResult, validate_json_documents,
)


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate a diagnosis against D0 evidence")
    parser.add_argument("evidence", type=Path)
    parser.add_argument("diagnosis", type=Path)
    args = parser.parse_args()
    try:
        result = validate_json_documents(
            args.evidence.read_text(encoding="utf-8"),
            args.diagnosis.read_text(encoding="utf-8"),
        )
    except (OSError, UnicodeError) as error:
        result = ValidationResult((ValidationError(
            ErrorCategory.SCHEMA_ERROR, "input", str(error),
        ),))
    print(json.dumps(result.to_dict(), indent=2))
    return 0 if result.valid else 1


if __name__ == "__main__":
    raise SystemExit(main())
