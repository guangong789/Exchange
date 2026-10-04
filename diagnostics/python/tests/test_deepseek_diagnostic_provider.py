from contextlib import redirect_stdout, redirect_stderr
from http.client import IncompleteRead
import io
import json
import os
from pathlib import Path
import socket
import ssl
import unittest
from unittest.mock import MagicMock, Mock, patch
from urllib.error import HTTPError, URLError

from diagnostics.python.deepseek_diagnostic_provider import (
    DEFAULT_MODEL, DEFAULT_TIMEOUT_SECONDS, MAX_RESPONSE_BYTES,
    DeepSeekDiagnosticProvider, _NoRedirect,
)
from diagnostics.python.diagnostic_provider import ProviderError, ProviderTimeoutError
from diagnostics.python.diagnostic_runner import RunStatus, run_diagnosis
from diagnostics.python.partial_fill_diagnosis import ErrorCategory
from diagnostics.python.run_diagnosis import main


FIXTURES = Path(__file__).parent / "fixtures"
TEST_KEY = "test-placeholder-not-a-real-api-key"


class DeepSeekDiagnosticProviderTest(unittest.TestCase):
    def setUp(self):
        # Every test intercepts HTTP, independently of the user's credentials.
        environment = patch.dict(os.environ, {"DEEPSEEK_API_KEY": TEST_KEY}, clear=True)
        environment.start()
        self.addCleanup(environment.stop)
        transport = patch("diagnostics.python.deepseek_diagnostic_provider.build_opener")
        self.build_opener = transport.start()
        self.addCleanup(transport.stop)
        self.opener = Mock()
        self.build_opener.return_value = self.opener
        self.response = MagicMock()
        self.response.__enter__.return_value = self.response
        self.response.status = 200
        self.opener.open.return_value = self.response
        self.evidence = (FIXTURES / "d0_partial_fill.json").read_text()
        self.valid = (FIXTURES / "valid_partial_fill_diagnosis.json").read_text()
        self.respond(self.valid)

    def response_body(self, body):
        self.response.read.side_effect = io.BytesIO(body).read

    def respond(self, content):
        body = json.dumps({"choices": [{"message": {
            "role": "assistant", "content": content,
        }}]}).encode("utf-8")
        self.response_body(body)
        return body

    def test_returns_assistant_text_unchanged(self):
        raw = " \n" + self.valid + "\n "
        self.respond(raw)
        self.assertEqual(DeepSeekDiagnosticProvider().complete("prompt"), raw)
        self.opener.open.assert_called_once()
        self.response.read.assert_called_once_with(MAX_RESPONSE_BYTES + 1)
        self.response.__exit__.assert_called_once()

    def test_request_is_one_nonstreaming_json_completion(self):
        DeepSeekDiagnosticProvider().complete("copied evidence prompt")
        request = self.opener.open.call_args.args[0]
        self.assertEqual(request.full_url, "https://api.deepseek.com/chat/completions")
        self.assertEqual(request.get_method(), "POST")
        self.assertEqual(request.get_header("Authorization"), "Bearer " + TEST_KEY)
        self.assertEqual(request.get_header("Content-type"), "application/json")
        body = json.loads(request.data)
        self.assertEqual(body["model"], DEFAULT_MODEL)
        self.assertEqual(body["messages"][0]["role"], "system")
        self.assertEqual(body["messages"][1], {
            "role": "user", "content": "copied evidence prompt",
        })
        self.assertEqual(body["temperature"], 0)
        self.assertEqual(body["thinking"], {"type": "disabled"})
        self.assertEqual(body["response_format"], {"type": "json_object"})
        self.assertEqual(body["max_tokens"], 1024)
        self.assertIs(body["stream"], False)
        self.assertNotIn("tools", body)
        self.assertNotIn("tool_choice", body)
        self.assertNotIn(TEST_KEY, request.data.decode())
        self.assertEqual(self.opener.open.call_args.kwargs, {"timeout": DEFAULT_TIMEOUT_SECONDS})
        self.assertIsInstance(self.build_opener.call_args.args[0], _NoRedirect)

    def test_environment_overrides(self):
        os.environ.update({"DEEPSEEK_BASE_URL": "https://example.invalid/v1/",
                           "DEEPSEEK_MODEL": "test-model",
                           "DEEPSEEK_TIMEOUT_SECONDS": "2.5"})
        DeepSeekDiagnosticProvider().complete("prompt")
        request = self.opener.open.call_args.args[0]
        self.assertEqual(request.full_url, "https://example.invalid/v1/chat/completions")
        self.assertEqual(json.loads(request.data)["model"], "test-model")
        self.assertEqual(self.opener.open.call_args.kwargs["timeout"], 2.5)

    def test_missing_or_invalid_key_fails_before_network(self):
        for value in (None, "", " ", "bad\nkey", "bad key", "非ASCII"):
            with self.subTest(value=value):
                if value is None:
                    os.environ.pop("DEEPSEEK_API_KEY", None)
                else:
                    os.environ["DEEPSEEK_API_KEY"] = value
                result = run_diagnosis(self.evidence, DeepSeekDiagnosticProvider())
                self.assertEqual(result.status, RunStatus.PROVIDER_ERROR)
                self.build_opener.assert_not_called()

    def test_invalid_configuration_fails_before_network(self):
        for name, value in (
            ("DEEPSEEK_TIMEOUT_SECONDS", "0"), ("DEEPSEEK_TIMEOUT_SECONDS", "-1"),
            ("DEEPSEEK_TIMEOUT_SECONDS", "nan"), ("DEEPSEEK_TIMEOUT_SECONDS", "inf"),
            ("DEEPSEEK_TIMEOUT_SECONDS", "not-a-number"), ("DEEPSEEK_MODEL", " "),
            ("DEEPSEEK_BASE_URL", "http://example.invalid"),
            ("DEEPSEEK_BASE_URL", "https://user:password@example.invalid"),
            ("DEEPSEEK_BASE_URL", "https://example.invalid?key=do-not-log"),
            ("DEEPSEEK_BASE_URL", "https://example.invalid#fragment"),
            ("DEEPSEEK_BASE_URL", "https://example.invalid:bad"),
            ("DEEPSEEK_BASE_URL", ""),
        ):
            with self.subTest(name=name, value=value), patch.dict(os.environ, {name: value}):
                with self.assertRaises(ProviderError):
                    DeepSeekDiagnosticProvider().complete("prompt")
                self.build_opener.assert_not_called()

    def test_invalid_response_bound_fails_before_network(self):
        for limit in (0, -1, True, 1.5):
            with self.subTest(limit=limit), self.assertRaises(ProviderError):
                DeepSeekDiagnosticProvider(limit).complete("prompt")
        self.build_opener.assert_not_called()

    def test_direct_and_url_wrapped_timeouts_map_to_existing_exception(self):
        for failure in (TimeoutError("timeout"), URLError(socket.timeout("timeout"))):
            with self.subTest(failure=type(failure).__name__):
                self.opener.open.reset_mock()
                self.opener.open.side_effect = failure
                with self.assertRaises(ProviderTimeoutError):
                    DeepSeekDiagnosticProvider().complete("prompt")
                self.opener.open.assert_called_once()

    def test_read_timeout_maps_to_runner_timeout_without_retry(self):
        self.response.read.side_effect = socket.timeout("read timeout")
        result = run_diagnosis(self.evidence, DeepSeekDiagnosticProvider())
        self.assertEqual(result.status, RunStatus.PROVIDER_TIMEOUT)
        self.assertIsNone(result.diagnosis)
        self.assertIsNone(result.raw_provider_output)
        self.opener.open.assert_called_once()
        self.response.__exit__.assert_called_once()

    def test_http_failures_are_provider_errors_and_do_not_expose_server_body(self):
        for code in (301, 401, 429, 500):
            with self.subTest(code=code):
                self.opener.open.reset_mock()
                server_body = io.BytesIO(TEST_KEY.encode())
                self.opener.open.side_effect = HTTPError(
                    "https://example.invalid", code, TEST_KEY, {}, server_body,
                )
                with self.assertRaises(ProviderError) as raised:
                    DeepSeekDiagnosticProvider().complete("prompt")
                self.assertNotIsInstance(raised.exception, ProviderTimeoutError)
                self.assertNotIn(TEST_KEY, str(raised.exception))
                self.assertIsNone(raised.exception.__cause__)
                self.assertTrue(server_body.closed)
                self.opener.open.assert_called_once()

    def test_non_success_response_is_rejected_before_read(self):
        self.response.status = 503
        with self.assertRaises(ProviderError):
            DeepSeekDiagnosticProvider().complete("prompt")
        self.response.read.assert_not_called()

    def test_dns_tls_and_network_failures_map_to_provider_error(self):
        for failure in (
            URLError(socket.gaierror("DNS failure")),
            URLError(ssl.SSLError("TLS failure")),
            ConnectionResetError("connection reset"),
        ):
            with self.subTest(failure=type(failure).__name__):
                self.opener.open.reset_mock()
                self.opener.open.side_effect = failure
                result = run_diagnosis(self.evidence, DeepSeekDiagnosticProvider())
                self.assertEqual(result.status, RunStatus.PROVIDER_ERROR)
                self.opener.open.assert_called_once()

    def test_incomplete_response_maps_to_provider_error(self):
        self.response.read.side_effect = IncompleteRead(b"partial")
        with self.assertRaises(ProviderError):
            DeepSeekDiagnosticProvider().complete("prompt")

    def test_malformed_envelope_and_missing_assistant_content(self):
        for envelope in (
            None, [], {}, {"choices": []}, {"choices": {}},
            {"choices": [None]}, {"choices": [{}, {}]},
            {"choices": [{}]}, {"choices": [{"message": []}]},
            {"choices": [{"message": {"role": "user", "content": "text"}}]},
            {"choices": [{"message": {"role": "assistant"}}]},
            {"choices": [{"message": {"role": "assistant", "content": None}}]},
            {"choices": [{"message": {"role": "assistant", "content": 42}}]},
            {"choices": [{"message": {"role": "assistant", "content": " "}}]},
        ):
            with self.subTest(envelope=envelope):
                self.response_body(json.dumps(envelope).encode())
                with self.assertRaises(ProviderError):
                    DeepSeekDiagnosticProvider().complete("prompt")

    def test_non_json_or_non_utf8_envelope_is_provider_error(self):
        for body in (b"{", b"\xff"):
            with self.subTest(body=body):
                self.response_body(body)
                with self.assertRaises(ProviderError):
                    DeepSeekDiagnosticProvider().complete("prompt")

    def test_oversized_response_is_bounded_and_rejected(self):
        self.response_body(b"x" * (MAX_RESPONSE_BYTES + 1000))
        result = run_diagnosis(self.evidence, DeepSeekDiagnosticProvider())
        self.assertEqual(result.status, RunStatus.PROVIDER_ERROR)
        self.response.read.assert_called_once_with(MAX_RESPONSE_BYTES + 1)
        self.assertIsNone(result.raw_provider_output)

    def test_response_exactly_at_bound_is_allowed(self):
        body = self.respond(self.valid)
        self.assertEqual(DeepSeekDiagnosticProvider(len(body)).complete("prompt"), self.valid)
        self.response.read.assert_called_once_with(len(body) + 1)

    def test_real_provider_result_still_requires_existing_parser(self):
        for raw in ('{"diagnosis_type":', "```json\n" + self.valid + "\n```",
                    '{"diagnosis_type":"partial_fill"}'):
            with self.subTest(raw=raw):
                self.respond(raw)
                result = run_diagnosis(self.evidence, DeepSeekDiagnosticProvider())
                self.assertEqual(result.status, RunStatus.PARSE_ERROR)
                self.assertEqual(result.raw_provider_output, raw)

    def test_real_provider_result_still_requires_existing_grounding(self):
        proposal = json.loads(self.valid)
        proposal["facts"]["matched_quantity"] = 4
        self.respond(json.dumps(proposal))
        result = run_diagnosis(self.evidence, DeepSeekDiagnosticProvider())
        self.assertEqual(result.status, RunStatus.VALIDATION_REJECTED)
        self.assertEqual(result.errors[0].category, ErrorCategory.CONTRADICTORY_FACT)

    def test_grounded_response_is_accepted_through_unchanged_runner(self):
        result = run_diagnosis(self.evidence, DeepSeekDiagnosticProvider())
        self.assertEqual(result.status, RunStatus.ACCEPTED)
        self.assertEqual(result.diagnosis.facts.order_id, 3)

    def test_unexpected_internal_errors_are_not_translated(self):
        failure = RuntimeError("programming bug")
        self.opener.open.side_effect = failure
        with self.assertRaises(RuntimeError) as raised:
            DeepSeekDiagnosticProvider().complete("prompt")
        self.assertIs(raised.exception, failure)

    def test_redirect_handler_never_forwards_credentials(self):
        self.assertIsNone(_NoRedirect().redirect_request(
            Mock(), Mock(), 302, "redirect", {}, "https://other.invalid",
        ))

    def run_cli(self, *arguments):
        output = io.StringIO()
        with patch("sys.argv", ["run_diagnosis", str(FIXTURES / "d0_partial_fill.json"),
                                *arguments]), redirect_stdout(output):
            code = main()
        return code, json.loads(output.getvalue())

    def test_cli_explicit_deepseek_path_uses_mocked_transport(self):
        code, output = self.run_cli("--provider", "deepseek")
        self.assertEqual(code, 0)
        self.assertEqual(output["status"], "accepted")
        self.opener.open.assert_called_once()

    def test_cli_missing_key_returns_provider_error_without_network(self):
        os.environ.pop("DEEPSEEK_API_KEY")
        code, output = self.run_cli("--provider", "deepseek")
        self.assertEqual(code, 1)
        self.assertEqual(output["status"], "provider_error")
        self.build_opener.assert_not_called()

    def test_cli_requires_explicit_provider_or_fake_configuration(self):
        for arguments in ([], ["--provider", "deepseek", "--failure", "timeout"]):
            with self.subTest(arguments=arguments), redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as raised:
                    self.run_cli(*arguments)
                self.assertEqual(raised.exception.code, 2)
        self.build_opener.assert_not_called()


if __name__ == "__main__":
    unittest.main()
