"""Raw-text provider boundary; D1-B supplies only an offline fake."""

from dataclasses import dataclass
from typing import Protocol


class ProviderError(Exception):
    """A declared provider failure, distinct from an internal programming error."""


class ProviderTimeoutError(ProviderError):
    """The provider did not return a response within its time budget."""


class DiagnosticProvider(Protocol):
    def complete(self, prompt: str) -> str:
        """Return untrusted raw text, or raise a declared provider error."""
        ...


@dataclass(frozen=True)
class FakeDiagnosticProvider:
    """Return exactly the configured text or raise a configured failure.

    No parsing, validation, sleeping, networking, or reasoning over the prompt.
    """

    raw_output: str = ""
    failure: ProviderError | None = None

    def complete(self, prompt: str) -> str:
        if self.failure is not None:
            raise self.failure
        return self.raw_output
