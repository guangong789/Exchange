"""One bounded HTTPS completion; returned model text remains untrusted."""

from dataclasses import dataclass
from http.client import HTTPException
import json
import math
import os
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener

from .diagnostic_provider import ProviderError, ProviderTimeoutError


DEFAULT_BASE_URL = "https://api.deepseek.com"
DEFAULT_MODEL = "deepseek-flash"
DEFAULT_TIMEOUT_SECONDS = 10.0
MAX_RESPONSE_BYTES = 64 * 1024


class _NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        # Do not send credentials to a redirect destination or issue a second request.
        return None


@dataclass(frozen=True)
class DeepSeekDiagnosticProvider:
    """Read DEEPSEEK_* settings at completion time, without storing the API key.

    timeout is the transport's socket-operation timeout, not a total wall-clock
    deadline. No application-level timers or retry wrappers are used.
    """

    max_response_bytes: int = MAX_RESPONSE_BYTES

    def complete(self, prompt: str) -> str:
        key = os.environ.get("DEEPSEEK_API_KEY", "").strip()
        if not key or not key.isascii() or any(char.isspace() for char in key):
            raise ProviderError("DEEPSEEK_API_KEY is missing or invalid")
        base_url = os.environ.get("DEEPSEEK_BASE_URL", DEFAULT_BASE_URL).strip()
        model = os.environ.get("DEEPSEEK_MODEL", DEFAULT_MODEL).strip()
        try:
            url = urlsplit(base_url)
            port = url.port
            if (url.scheme != "https" or not url.hostname or url.username is not None
                    or url.password is not None or url.query or url.fragment
                    or (port is not None and port <= 0)):
                raise ValueError()
            timeout = float(os.environ.get(
                "DEEPSEEK_TIMEOUT_SECONDS", str(DEFAULT_TIMEOUT_SECONDS),
            ))
            if not math.isfinite(timeout) or timeout <= 0:
                raise ValueError()
        except ValueError:
            raise ProviderError("Invalid DeepSeek URL or timeout configuration") from None
        if not model:
            raise ProviderError("DEEPSEEK_MODEL must be nonempty")
        if type(self.max_response_bytes) is not int or self.max_response_bytes <= 0:
            raise ProviderError("DeepSeek response size limit must be a positive integer")

        request = Request(
            base_url.rstrip("/") + "/chat/completions",
            data=json.dumps({
                "model": model,
                "messages": [
                    {"role": "system", "content": "Return only JSON grounded in the supplied evidence."},
                    {"role": "user", "content": prompt},
                ],
                "temperature": 0,
                "thinking": {"type": "disabled"},
                "response_format": {"type": "json_object"},
                "max_tokens": 1024,
                "stream": False,
            }).encode("utf-8"),
            headers={"Content-Type": "application/json", "Authorization": "Bearer " + key},
            method="POST",
        )
        try:
            with build_opener(_NoRedirect()).open(request, timeout=timeout) as response:
                if not 200 <= response.status < 300:
                    raise ProviderError("DeepSeek returned a non-success HTTP status")
                body = response.read(self.max_response_bytes + 1)
        except HTTPError as error:
            error.close()
            # Never surface server error bodies, request headers, or exception detail.
            raise ProviderError("DeepSeek returned a non-success HTTP status") from None
        except URLError as error:
            if isinstance(error.reason, TimeoutError):
                raise ProviderTimeoutError("DeepSeek transport timed out") from None
            raise ProviderError("DeepSeek transport failed") from None
        except TimeoutError:
            raise ProviderTimeoutError("DeepSeek transport timed out") from None
        except (OSError, HTTPException):
            raise ProviderError("DeepSeek transport failed") from None
        if len(body) > self.max_response_bytes:
            raise ProviderError("DeepSeek response exceeded the size limit")

        try:
            envelope = json.loads(body.decode("utf-8"))
        except (UnicodeError, ValueError, RecursionError):
            raise ProviderError("DeepSeek returned malformed response JSON") from None
        choices = envelope.get("choices") if isinstance(envelope, dict) else None
        if not isinstance(choices, list) or len(choices) != 1 or not isinstance(choices[0], dict):
            raise ProviderError("DeepSeek response has invalid choices")
        message = choices[0].get("message")
        if not isinstance(message, dict) or message.get("role") != "assistant":
            raise ProviderError("DeepSeek response has no assistant message")
        content = message.get("content")
        if not isinstance(content, str) or not content.strip():
            raise ProviderError("DeepSeek response has no assistant content")
        return content
