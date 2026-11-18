from __future__ import annotations

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import threading
import unittest
import urllib.error
import urllib.parse
import urllib.request

from gateway.extract import GatewayError
from gateway.server import RGB565_CONTENT_TYPE, create_server, pillow_available, render_rgb565

if pillow_available():
    from PIL import Image


class _FixtureHandler(BaseHTTPRequestHandler):
    image_png = b""

    def do_GET(self) -> None:
        pages = {
            "/article": (
                b"text/html; charset=utf-8",
                "<html><head><title>Fixture article</title></head><body><main>"
                "<h1>News</h1><p>First &amp; foremost.</p><p>Second paragraph.</p>"
                "</main></body></html>".encode(),
            ),
            "/script": (
                b"text/html; charset=utf-8",
                b"<html><head><script>required()</script></head><body><main></main></body></html>",
            ),
        }
        if self.path in pages:
            content_type, body = pages[self.path]
            self.send_response(200)
            self.send_header("Content-Type", content_type.decode())
        elif self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/article")
            body = b""
        elif self.path == "/image.png":
            self.send_response(200)
            self.send_header("Content-Type", "image/png")
            body = self.image_png
        else:
            self.send_response(404)
            self.send_header("Content-Type", "text/plain")
            body = b"Not found"
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format: str, *args: object) -> None:
        return


def _make_image() -> bytes:
    image = Image.new("RGB", (4, 4))
    colors = ((255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255))
    image.putdata([color for color in colors for _ in range(4)])
    output = io.BytesIO()
    image.save(output, format="PNG")
    image.close()
    return output.getvalue()


class GatewayHTTPTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if pillow_available():
            _FixtureHandler.image_png = _make_image()
        cls.fixture = ThreadingHTTPServer(("127.0.0.1", 0), _FixtureHandler)
        cls.fixture_thread = threading.Thread(target=cls.fixture.serve_forever, daemon=True)
        cls.fixture_thread.start()
        cls.gateway = create_server(port=0, allow_private_hosts=True)
        cls.gateway_thread = threading.Thread(target=cls.gateway.serve_forever, daemon=True)
        cls.gateway_thread.start()
        cls.fixture_url = f"http://127.0.0.1:{cls.fixture.server_address[1]}"
        cls.gateway_url = f"http://127.0.0.1:{cls.gateway.server_address[1]}"

    @classmethod
    def tearDownClass(cls) -> None:
        for server, thread in (
            (cls.gateway, cls.gateway_thread),
            (cls.fixture, cls.fixture_thread),
        ):
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

    def reader(self, source_url: str) -> tuple[int, dict[str, object], object]:
        target = self.gateway_url + "/v1/reader?" + urllib.parse.urlencode({"url": source_url})
        return self.request(target)

    def request(self, target: str, data: bytes | None = None) -> tuple[int, dict, object]:
        headers = {"Content-Type": "application/json"} if data is not None else {}
        request = urllib.request.Request(target, data=data, headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=5) as response:
                return response.status, dict(response.headers), response.read()
        except urllib.error.HTTPError as error:
            with error:
                return error.code, dict(error.headers), error.read()

    def post(self, value: dict[str, object]) -> tuple[int, dict[str, object], object]:
        return self.request(
            self.gateway_url + "/v1/interactive", json.dumps(value).encode("utf-8")
        )

    def test_health_reader_and_redirect_use_real_http(self) -> None:
        status, headers, body = self.request(self.gateway_url + "/health")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body), {"status": "ok"})
        self.assertNotIn("Access-Control-Allow-Origin", headers)

        status, _, body = self.reader(self.fixture_url + "/article")
        self.assertEqual(status, 200)
        document = json.loads(body)
        self.assertEqual(document["source_url"], self.fixture_url + "/article")
        self.assertEqual(document["mode"], "reader")
        self.assertIsNone(document["session"])
        self.assertEqual(
            [block.get("text") for block in document["blocks"]],
            ["News", "First & foremost.", "Second paragraph."],
        )

        status, _, body = self.reader(self.fixture_url + "/redirect")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)["source_url"], self.fixture_url + "/article")

    def test_script_only_fixture_is_http_200_unsupported_reader(self) -> None:
        status, _, body = self.reader(self.fixture_url + "/script")
        self.assertEqual(status, 200)
        document = json.loads(body)
        self.assertEqual(document["kind"], "unsupported")
        self.assertTrue(document["interactive_available"])
        self.assertIn("interactive mode", document["blocks"][0]["text"])

    @unittest.skipUnless(pillow_available(), "Pillow is not installed")
    def test_asset_returns_scaled_output_rgb565_strip_and_metadata(self) -> None:
        query = urllib.parse.urlencode({
            "url": self.fixture_url + "/image.png", "width": 4, "y": 1, "height": 2
        })
        status, headers, body = self.request(self.gateway_url + "/v1/asset?" + query)
        self.assertEqual(status, 200, body)
        self.assertEqual(headers["Content-Type"], RGB565_CONTENT_TYPE)
        self.assertEqual(headers["X-Microbrowser-Width"], "4")
        self.assertEqual(headers["X-Microbrowser-Height"], "2")
        self.assertEqual(headers["X-Microbrowser-Total-Height"], "4")
        self.assertEqual(headers["X-Microbrowser-Y"], "1")
        self.assertEqual(len(body), 4 * 2 * 2)
        self.assertEqual(body, bytes.fromhex("e007" * 4 + "1f00" * 4))

    @unittest.skipUnless(pillow_available(), "Pillow is not installed")
    def test_asset_rejects_height_beyond_reader_budget(self) -> None:
        with Image.new("RGB", (8, 32_769)) as image:
            output = io.BytesIO()
            image.save(output, format="PNG")
        with self.assertRaises(GatewayError) as rejected:
            render_rgb565(output.getvalue(), width=8, y=0, height=640)
        self.assertEqual(rejected.exception.code, "image_too_large")

    def test_asset_rejects_out_of_bounds_width(self) -> None:
        query = urllib.parse.urlencode({
            "url": self.fixture_url + "/image.png", "width": 481
        })
        status, _, body = self.request(self.gateway_url + "/v1/asset?" + query)
        self.assertEqual(status, 400)
        self.assertEqual(json.loads(body)["error"]["code"], "invalid_request")

    def test_missing_or_false_consent_never_starts_chromium(self) -> None:
        for value in ({"url": self.fixture_url + "/article"},
                      {"url": self.fixture_url + "/article", "consent": False}):
            with self.subTest(value=value):
                status, _, body = self.post(value)
                self.assertEqual(status, 403)
                self.assertEqual(json.loads(body)["error"]["code"], "consent_required")

    def test_interactive_never_uses_private_fixture_override(self) -> None:
        status, _, body = self.post({
            "url": self.fixture_url + "/article", "consent": True
        })
        self.assertEqual(status, 403)
        self.assertEqual(json.loads(body)["error"]["code"], "blocked_origin")

    def test_default_gateway_blocks_private_reader_origin(self) -> None:
        gateway = create_server(port=0)
        thread = threading.Thread(target=gateway.serve_forever, daemon=True)
        thread.start()
        try:
            target = f"http://127.0.0.1:{gateway.server_address[1]}/v1/reader?" + urllib.parse.urlencode({
                "url": self.fixture_url + "/article"
            })
            status, _, body = self.request(target)
            self.assertEqual(status, 403)
            self.assertEqual(json.loads(body)["error"]["code"], "blocked_origin")
        finally:
            gateway.shutdown()
            gateway.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    unittest.main()
