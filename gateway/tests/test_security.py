from __future__ import annotations

import http.server
import unittest
import urllib.request

from gateway.extract import GatewayError, URLPolicy, _PolicyRedirectHandler


class URLSecurityTests(unittest.TestCase):
    def test_rejects_non_http_credentials_and_non_global_literal_ips(self) -> None:
        blocked = (
            "file:///etc/passwd",
            "ftp://example.com/file",
            "https://user:password@example.com/",
            "http://127.0.0.1:8765/",
            "http://10.0.0.8/",
            "http://169.254.169.254/latest/meta-data",
            "http://[::1]/",
            "https://localhost/",
        )
        for url in blocked:
            with self.subTest(url=url), self.assertRaises(GatewayError):
                URLPolicy().validate(url)

    def test_checks_dns_resolution_when_host_is_not_an_ip(self) -> None:
        # .localhost is rejected without performing a network request.
        with self.assertRaises(GatewayError) as caught:
            URLPolicy().validate("https://anything.localhost/")
        self.assertEqual(caught.exception.code, "blocked_origin")

    def test_redirect_destination_is_validated_before_redirect(self) -> None:
        handler = _PolicyRedirectHandler(URLPolicy())
        request = urllib.request.Request("https://example.com/start")
        with self.assertRaises(GatewayError) as caught:
            handler.redirect_request(
                request,
                object(),
                http.HTTPStatus.FOUND,
                "Found",
                {},
                "http://127.0.0.1:8765/private",
            )
        self.assertEqual(caught.exception.code, "blocked_origin")

    def test_development_switch_keeps_scheme_and_credential_checks(self) -> None:
        policy = URLPolicy(allow_private_hosts=True)
        self.assertEqual(policy.validate("http://127.0.0.1:8765/"), "http://127.0.0.1:8765/")
        for url in ("file:///etc/passwd", "http://user@127.0.0.1:8765/"):
            with self.subTest(url=url), self.assertRaises(GatewayError):
                policy.validate(url)


if __name__ == "__main__":
    unittest.main()
