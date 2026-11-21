from __future__ import annotations

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import subprocess
import sys
import threading
import unittest

from gateway.extract import GatewayError, URLPolicy
from gateway.interactive import (
    InteractiveService,
    find_chromium_executable,
    playwright_available,
)


class InteractiveDependencyTests(unittest.TestCase):
    def test_missing_playwright_is_reported_without_importing_site_packages(self) -> None:
        project_root = Path(__file__).resolve().parents[2]
        script = f"""
import sys
sys.path.insert(0, {str(project_root)!r})
from gateway.extract import GatewayError
from gateway.interactive import playwright_available, require_playwright
assert not playwright_available()
try:
    require_playwright()
except GatewayError as error:
    assert error.code == 'playwright_unavailable'
    assert error.status == 503
else:
    raise AssertionError('Missing Playwright was not reported')
"""
        environment = os.environ.copy()
        environment.pop("PYTHONPATH", None)
        result = subprocess.run(
            [sys.executable, "-S", "-c", script],
            cwd=project_root,
            capture_output=True,
            text=True,
            timeout=15,
            check=False,
            env=environment,
        )
        self.assertEqual(result.returncode, 0, result.stderr)


class _InteractiveFixture(BaseHTTPRequestHandler):
    def do_GET(self) -> None:
        pages = {
            "/": b"""<!doctype html><html><head><title>Dynamic fixture</title></head>
                <body><main></main><script>
                const main = document.querySelector('main');
                main.innerHTML = '<h1>Dynamic content</h1><p>Rendered after script.</p>' +
                  '<a href="/next">Follow returned anchor</a>';
                const gl = document.createElement('canvas').getContext('webgl');
                const state = document.createElement('p');
                state.textContent = gl === null ? 'WebGL disabled' : 'WebGL enabled';
                const socketState = document.createElement('p');
                socketState.textContent = 'WebSocket pending';
                main.appendChild(socketState);
                try {
                  const socket = new WebSocket(`ws://${location.host}/socket`);
                  socket.onopen = () => socketState.textContent = 'WebSocket opened';
                  socket.onclose = () => socketState.textContent = 'WebSocket closed';
                } catch (error) {
                  socketState.textContent = 'WebSocket closed';
                }
                const popup = window.open('/popup', '_blank');
                const popupState = document.createElement('p');
                main.appendChild(popupState);
                setTimeout(() => popupState.textContent = popup && !popup.closed ? 'Popup open' : 'Popup closed', 100);
                main.appendChild(state);
                </script></body></html>""",
            "/pagination": b"<html><head><link rel='next' href='/after-pagination'></head><body><main><p>First result</p></main></body></html>",
            "/after-pagination": b"<html><body><main><p>Second result</p></main></body></html>",
            "/popup": b"<html><body><main><p>Unallowed popup</p></main></body></html>",
            "/next": b"<html><body><main><h1>Next page</h1><p>Same browser context.</p></main></body></html>",
            "/not-returned": b"<html><body><main><p>This must not be opened through the old session.</p></main></body></html>",
            "/sensitive": b"<html><body><main><p>Sign in</p><form><input type='password'></form></main></body></html>",
        }
        type(self).socket_hits = getattr(type(self), 'socket_hits', 0) + int(self.path == '/socket')
        body = pages.get(self.path, pages["/not-returned"])
        self.send_response(200 if self.path in pages else 404)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format: str, *args: object) -> None:
        return


@unittest.skipUnless(
    playwright_available() and find_chromium_executable() is not None,
    "Playwright and a usable Chromium executable are required",
)
class InteractiveChromiumTests(unittest.TestCase):
    def test_dynamic_content_session_anchor_navigation_and_sensitive_rejection(self) -> None:
        _InteractiveFixture.socket_hits = 0
        fixture = ThreadingHTTPServer(("127.0.0.1", 0), _InteractiveFixture)
        thread = threading.Thread(target=fixture.serve_forever, daemon=True)
        thread.start()
        base_url = f"http://127.0.0.1:{fixture.server_address[1]}"
        service = InteractiveService(URLPolicy(allow_private_hosts=True))
        try:
            document = service.navigate(base_url + "/", True)
            self.assertEqual(document["mode"], "interactive")
            self.assertIsInstance(document["session"], str)
            self.assertGreaterEqual(len(document["session"]), 32)
            texts = [block.get("text") for block in document["blocks"]]
            self.assertIn("Dynamic content", texts)
            self.assertIn("Rendered after script.", texts)
            self.assertIn("WebGL disabled", texts)
            link = next(block for block in document["blocks"] if block["type"] == "link")
            self.assertIn("WebSocket closed", texts)
            self.assertIn("Popup closed", texts)
            self.assertEqual(_InteractiveFixture.socket_hits, 0)
            self.assertEqual(link["url"], base_url + "/next")

            with self.assertRaises(GatewayError) as rejected:
                service.navigate(base_url + "/not-returned", True, document["session"])
            self.assertEqual(rejected.exception.code, "action_not_allowed")

            following = service.navigate(link["url"], True, document["session"])
            self.assertEqual(following["session"], document["session"])
            self.assertEqual(following["source_url"], base_url + "/next")
            self.assertIn("Next page", [block.get("text") for block in following["blocks"]])

            paginated = service.navigate(base_url + "/pagination", True)
            if not isinstance(paginated["next_url"], str) or not isinstance(paginated["session"], str):
                self.fail("paginated interactive document omitted its session or next-page URL")
            following_page = service.navigate(paginated["next_url"], True, paginated["session"])
            self.assertIn("Second result", [block.get("text") for block in following_page["blocks"]])
            with self.assertRaises(GatewayError) as sensitive:
                service.navigate(base_url + "/sensitive", True)
            self.assertEqual(sensitive.exception.code, "sensitive_content")
        finally:
            service.close()
            fixture.shutdown()
            fixture.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    unittest.main()
