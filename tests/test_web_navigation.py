"""End-to-end website loading through the real gateway and desktop runtime."""

from __future__ import annotations

import io
import json
import socket
import subprocess
import sys
import threading
import unittest
import urllib.request
import urllib.parse
import tempfile
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from gateway import server as gateway_server
from tools.publish_reader import publish_sources

ROOT = Path(__file__).resolve().parents[1]
CAPTURES = ROOT / "build" / "reader-captures"


def _png(width: int, height: int):
    try:
        from PIL import Image, ImageDraw
    except ImportError as error:
        raise unittest.SkipTest("long-image conversion requires Pillow") from error
    image = Image.new("RGB", (width, height), "white")
    draw = ImageDraw.Draw(image)
    for panel in range(3):
        top = panel * height // 3
        bottom = (panel + 1) * height // 3
        draw.rectangle((4, top + 4, width - 5, bottom - 5), fill=(35, 75 + panel * 40, 140), outline=(15, 30, 60), width=3)
        for offset in range(top + 36, bottom, 42):
            draw.ellipse((20, offset, 60, offset + 35), fill=(245, 210, 130))
            draw.text((70, offset + 10), f"Panel {panel + 1}, line {offset}", fill="white")
    encoded = io.BytesIO()
    image.save(encoded, format="PNG")
    return encoded.getvalue()


def chapter_html(title: str, paragraph: str, next_chapter: bool) -> bytes:
    link = '<a rel="next" href="/chapter-two">Read chapter two</a>' if next_chapter else '<a rel="prev" href="/chapter-one">Previous chapter</a>'
    return f"""<!doctype html><html><head><title>{title}</title></head><body>
<nav>Site navigation that Reader should omit</nav><main>
<h1>{title}</h1><p>{paragraph}</p>
<p>Another paragraph with <strong>bold text</strong>, an <em>emphasized phrase</em> and the entity &amp;.</p>
{link}</main><script>document.body.dataset.unused = 'never execute this';</script>
</body></html>""".encode()


class FixtureServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, routes: dict[str, bytes]):
        self.package_fetches = 0
        self.routes = routes
        super().__init__(("127.0.0.1", 0), FixtureHandler)


class FixtureHandler(BaseHTTPRequestHandler):
    def do_GET(self) -> None:
        path = self.path.partition("?")[0]
        body = self.server.routes.get(path)
        if body is None:
            self.send_error(404)
            return
        if path.endswith('.mbp'):
            self.server.package_fetches += 1
        content_type = "application/json" if path.endswith(".json") else "text/html; charset=utf-8"
        if path.endswith(".png"):
            content_type = "image/png"
        if path.endswith(".mbp"):
            content_type = "application/octet-stream"
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format: str, *args: object) -> None:
        pass


class WebNavigationTest(unittest.TestCase):
    def setUp(self) -> None:
        CAPTURES.mkdir(parents=True, exist_ok=True)
        static_document = {
            "format": "microbrowser-reader-v1",
            "kind": "article", "mode": "reader",
            "title": "Published static chapter",
            "source_url": "https://unrelated.example/canonical-source",
            "previous_url": None, "next_url": None,
            "interactive_available": False, "session": None,
            "blocks": [
                {"type": "heading", "text": "Published without a live gateway"},
                {"type": "paragraph", "text": "This Reader document was fetched directly from static hosting."},
            ],
        }
        package = ROOT / "build" / "counter.mbp"
        if not package.exists():
            subprocess.run([sys.executable, "tools/mbc", "build", "examples/counter.mb", "-o", str(package)], cwd=ROOT, check=True)
        self.routes = {
            "/chapter-one": chapter_html("Chapter One: Wind over water", "The real first chapter arrived through the URL loader.", True),
            "/chapter-two": chapter_html("Chapter Two: Across the bay", "The address input accepted this second chapter URL.", False),
            "/manga": b"<html><head><title>Three-panel chapter</title></head><body><main><h1>Three-panel chapter</h1><img src='/panel-strip.png' alt='Three illustrated panels'></main></body></html>",
            "/static.mbr.json": json.dumps(static_document).encode(),
            "/counter.mbp": package.read_bytes(),
        }
        self.site = FixtureServer(self.routes)
        self.gateway = gateway_server.create_server(port=0, allow_private_hosts=True)
        self.threads = [
            threading.Thread(target=self.site.serve_forever, daemon=True),
            threading.Thread(target=self.gateway.serve_forever, daemon=True),
        ]
        for thread in self.threads:
            thread.start()
        address = self.site.server_address[1]
        self.origin = f"http://127.0.0.1:{address}"
        self.gateway_origin = f"http://127.0.0.1:{self.gateway.server_address[1]}"
        self.publication_directory = tempfile.TemporaryDirectory(prefix="microbrowser-public-")
        self.addCleanup(self.publication_directory.cleanup)
        published = publish_sources([("published-chapter", self.url("/chapter-one"))],
                                    Path(self.publication_directory.name), self.origin + "/", True)
        self.routes["/static.mbr.json"] = (Path(self.publication_directory.name) / "published-chapter.mbr.json").read_bytes()
        self.routes["/index.mbr.json"] = (Path(self.publication_directory.name) / "index.mbr.json").read_bytes()
        self.assertEqual(len(published), 2)

    def tearDown(self) -> None:
        self.site.shutdown()
        self.gateway.shutdown()
        self.site.server_close()
        self.gateway.server_close()
        for thread in self.threads:
            thread.join(timeout=10)
        self.assertTrue(all(not thread.is_alive() for thread in self.threads))

    def url(self, path: str) -> str:
        return self.origin + path

    def _free_port(self) -> int:
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            return int(listener.getsockname()[1])

    def start_gateway_process(self, port: int):
        return subprocess.Popen(
            [sys.executable, "-m", "gateway.server", "--host", "127.0.0.1", "--port", str(port), "--allow-private-hosts"],
            cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )

    def replay(self, name: str, source: str, commands: list[str], gateway: str | None = None) -> str:
        events = ROOT / "build" / f"{name}.events"
        commands = commands + [f"shot {CAPTURES / (name + '.ppm')}"]
        events.write_text("\n".join(commands) + "\n", encoding="utf-8")
        waits = sum(int(line.split()[1]) for line in commands if line.startswith("wait "))
        frames = len(commands) + waits // 16 + 100
        args = [str(ROOT / "build" / "microbrowser"), "--url", source,
                "--gateway", gateway or self.gateway_origin,
                "--size", "480x320", "--headless", "--frames", str(frames),
                "--events", str(events)]
        completed = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=120)
        output = completed.stdout + completed.stderr
        (ROOT / "build" / f"{name}.log").write_text(output, encoding="utf-8")
        self.assertEqual(completed.returncode, 0, output)
        self.assertNotIn("Browser last error:", output)
        self.assertNotIn("script error", output)
        return output

    def test_address_bar_opens_typed_url_and_back_returns_to_chapter(self) -> None:
        first = self.url("/chapter-one")
        second = self.url("/chapter-two")
        events = ["wait 700", "click 210 21", "key end"]
        events.extend(["key backspace"] * len(first))
        events.extend(["text " + second, "key enter", "wait 900", f"shot {CAPTURES / 'chapter-two.ppm'}",
                       "click 16 21", "wait 700"])
        self.replay("chapter-navigation", first, events)
        for page in ("chapter-two.ppm", "chapter-navigation.ppm"):
            with self.subTest(page=page):
                self.assertGreater((CAPTURES / page).stat().st_size, 400_000)
        try:
            from PIL import Image
        except ImportError:
            return
        for page in ("chapter-two", "chapter-navigation"):
            with Image.open(CAPTURES / f"{page}.ppm") as capture:
                capture.convert("RGB").save(CAPTURES / f"{page}.png", optimize=True)

    def test_long_manga_image_loads_segment_and_scrolls_natively(self) -> None:
        self.routes["/panel-strip.png"] = _png(640, 2000)
        events = ["wait 1400", f"shot {CAPTURES / 'manga-first.ppm'}",
                  "scroll -150", "wait 1000"]
        output = self.replay("manga-navigation", self.url("/manga"), events)
        self.assertIn("scripts=0", output)
        try:
            from PIL import Image, ImageChops
        except ImportError:
            return
        for name in ("manga-first", "manga-navigation"):
            with Image.open(CAPTURES / f"{name}.ppm") as capture:
                capture.convert("RGB").save(CAPTURES / f"{name}.png", optimize=True)
        with Image.open(CAPTURES / "manga-first.ppm") as before, Image.open(CAPTURES / "manga-navigation.ppm") as after:
            difference = ImageChops.difference(before.convert("RGB"), after.convert("RGB"))
            changed_pixels = sum(1 for pixel in difference.getdata() if any(pixel))
            self.assertGreater(changed_pixels, 480 * 320 // 20)

    def test_direct_mbr_json_and_native_package_confirmation(self) -> None:
        self.replay("static-reader", self.url("/static.mbr.json"), ["wait 600"])
        package = self.url("/counter.mbp")
        abandoned = self.replay(
            "native-abandoned", package,
            ["wait 300", "click 16 21", "wait 100", "url " + package, "wait 300",
             f"shot {CAPTURES / 'native-package-confirmation.ppm'}"],
        )
        self.assertIn("Content: Native application download (reader)", abandoned)
        self.assertEqual(self.site.package_fetches, 0)

        output = self.replay(
            "native-package", package,
            ["wait 300", "click 240 183", "wait 700", "call increase", "call report", "wait 100",
             "url mb://home", "wait 100", "url " + package, "wait 700", "call report", "wait 100"],
        )
        self.assertIn("Counter value: 1", output)
        self.assertIn("Counter value: 0", output)
        self.assertIn("Content: home (application)", output)
        self.assertEqual(self.site.package_fetches, 2)
        self.assertNotRegex(output, r"script error|SELFTEST FAIL")

    def test_gateway_command_line_serves_reader_and_rgb565(self) -> None:
        self.routes["/panel-strip.png"] = _png(96, 900)
        port = self._free_port()
        process = self.start_gateway_process(port)
        try:
            origin = f"http://127.0.0.1:{port}"
            for _ in range(100):
                try:
                    with urllib.request.urlopen(origin + "/health", timeout=1) as response:
                        if response.status == 200:
                            break
                except OSError:
                    if process.poll() is not None:
                        self.fail("gateway command line exited: " + (process.stderr.read() if process.stderr else ""))
                    threading.Event().wait(0.05)
            else:
                self.fail("gateway command line did not become ready")
            source = f"url={urllib.request.quote(self.url('/manga'), safe='')}"
            with urllib.request.urlopen(origin + "/v1/reader?" + source, timeout=15) as response:
                document = json.load(response)
            self.assertEqual(document["mode"], "reader")
            self.assertTrue(document["blocks"])
            asset = origin + document["blocks"][-1]["url"] + "&width=96&y=640&height=640"
            with urllib.request.urlopen(asset, timeout=15) as response:
                body = response.read()
                self.assertEqual(response.headers["X-Microbrowser-Width"], "96")
                self.assertEqual(int(response.headers["X-Microbrowser-Height"]), 260)
                self.assertEqual(int(response.headers["X-Microbrowser-Total-Height"]), 900)
                self.assertEqual(len(body), 96 * 260 * 2)
        finally:
            process.terminate()
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=15)
            stdout, stderr = process.communicate()
            if process.returncode not in (0, -15):
                self.fail(f"gateway process failed: {stdout}\n{stderr}")


if __name__ == "__main__":
    unittest.main()
