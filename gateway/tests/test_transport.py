from __future__ import annotations

from collections.abc import Iterator
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import socket
import ssl
import subprocess
import tempfile
import urllib.error
import threading
import unittest
from unittest import mock
import urllib.request

from gateway.extract import FETCH_TIMEOUT_SECONDS, GatewayError, URLPolicy
from gateway.tcp_proxy import TransportProxy


PUBLIC_TEST_ADDRESS = "93.184.215.14"


class _FixtureServer(ThreadingHTTPServer):
    def __init__(self) -> None:
        self.received: list[str] = []
        super().__init__(("127.0.0.1", 0), _FixtureHandler)


class _FixtureHandler(BaseHTTPRequestHandler):
    server: _FixtureServer

    def do_GET(self) -> None:
        self.server.received.append(f"GET {self.path}")
        body = b"validated-https" if self.path == "/tls" else b"validated-http"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self) -> None:
        self.server.received.append(f"POST {self.path}")
        self.send_error(400)

    def log_message(self, format: str, *args: object) -> None:
        return


@contextmanager
def fixture(scheme: str = "http", tls: ssl.SSLContext | None = None) -> Iterator[tuple[_FixtureServer, str]]:
    server = _FixtureServer()
    if scheme == "https" and tls is not None:
        server.socket = tls.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    port = server.server_address[1]
    try:
        yield server, f"{scheme}://127.0.0.1:{port}"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


class TransportTests(unittest.TestCase):
    def test_connection_uses_the_validated_numeric_address(self) -> None:
        resolved = [(socket.AF_INET, socket.SOCK_STREAM, 6, "", (PUBLIC_TEST_ADDRESS, 443))]
        connection = mock.Mock(spec=socket.socket)
        connection.getpeername.return_value = (PUBLIC_TEST_ADDRESS, 443)
        with (
            mock.patch("gateway.extract.socket.getaddrinfo", return_value=resolved),
            mock.patch("gateway.extract.socket.create_connection", return_value=connection) as connect,
        ):
            result = URLPolicy().connect("https://example.com/")
        self.assertIs(result, connection)
        connect.assert_called_once_with((PUBLIC_TEST_ADDRESS, 443), timeout=FETCH_TIMEOUT_SECONDS)

    def test_private_address_in_dns_results_prevents_every_connection(self) -> None:
        resolved = [(socket.AF_INET, socket.SOCK_STREAM, 6, "", (PUBLIC_TEST_ADDRESS, 443)),
                    (socket.AF_INET, socket.SOCK_STREAM, 6, "", ("127.0.0.1", 443))]
        with (
            mock.patch("gateway.extract.socket.getaddrinfo", return_value=resolved),
            mock.patch("gateway.extract.socket.create_connection") as connect,
            self.assertRaises(GatewayError) as rejected,
        ):
            URLPolicy().connect("https://example.com/")
        self.assertEqual(rejected.exception.code, "blocked_origin")
        connect.assert_not_called()

    def test_public_connect_cannot_tunnel_to_a_non_https_port(self) -> None:
        with mock.patch("socket.create_connection") as connect, self.assertRaises(GatewayError) as rejected:
            URLPolicy().connect("https://example.com:22/")
        self.assertEqual(rejected.exception.code, "blocked_origin")
        connect.assert_not_called()

    def test_loopback_http_works_and_rejects_unallowed_requests(self) -> None:
        policy = URLPolicy(allow_private_hosts=True)
        with fixture() as (server, origin), TransportProxy(policy.connect) as proxy:
            opener = urllib.request.build_opener(urllib.request.ProxyHandler({"http": proxy.url}))
            with opener.open(origin + "/document?part=one", timeout=5) as response:
                self.assertEqual(response.read(), b"validated-http")
            with self.assertRaises(urllib.error.HTTPError) as rejected:
                opener.open(urllib.request.Request(origin + "/", data=b"blocked", method="POST"), timeout=5)
            with rejected.exception:
                self.assertEqual(rejected.exception.code, 405)
            self.assertEqual(server.received, ["GET /document?part=one"])
            with self.assertRaises(urllib.error.HTTPError) as mismatched:
                opener.open(urllib.request.Request(origin + "/", headers={"Host": "victim.example"}), timeout=5)
            with mismatched.exception:
                self.assertEqual(mismatched.exception.code, 400)
            self.assertEqual(len(server.received), 1)

    def test_connect_preserves_tls_certificate_verification(self) -> None:
        with tempfile.TemporaryDirectory(prefix="microbrowser-tls-test-") as directory:
            certificate = Path(directory) / "certificate.pem"
            key = Path(directory) / "key.pem"
            subprocess.run(
                [
                    "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                    "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1",
                    "-keyout", str(key), "-out", str(certificate),
                ],
                check=True,
                capture_output=True,
                timeout=10,
            )
            client_tls = ssl.create_default_context(cafile=str(certificate))
            server_tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            server_tls.load_cert_chain(certificate, key)
            policy = URLPolicy(allow_private_hosts=True)
            with (
                fixture("https", server_tls) as (_, origin),
                TransportProxy(policy.connect) as proxy,
            ):
                opener = urllib.request.build_opener(
                    urllib.request.ProxyHandler({"https": proxy.url}),
                    urllib.request.HTTPSHandler(context=client_tls),
                )
                with opener.open(origin + "/tls", timeout=10) as response:
                    self.assertEqual(response.read(), b"validated-https")


if __name__ == "__main__":
    unittest.main()
