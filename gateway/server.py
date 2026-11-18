"""Local HTTP gateway for Reader extraction, image strips, and interactive pages."""

from __future__ import annotations

import argparse
from collections.abc import Callable
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import logging
import threading
from typing import TYPE_CHECKING
import urllib.parse

from .extract import GatewayError, JsonObject, URLPolicy, extract_html, fetch
from .interactive import InteractiveService

if TYPE_CHECKING:
    from PIL.Image import Image as PILImage

try:
    from PIL import Image
except ImportError:
    Image = None

LOGGER = logging.getLogger("microbrowser.gateway")
MAX_REQUEST_JSON_BYTES = 16 * 1024
MAX_IMAGE_DOWNLOAD_BYTES = 16 * 1024 * 1024
MAX_SOURCE_PIXELS = 20_000_000
MAX_SOURCE_DIMENSION = 32_768
MAX_OUTPUT_WIDTH = 480
MAX_STRIP_HEIGHT = 640
MAX_OUTPUT_HEIGHT = 32_768
RGB565_CONTENT_TYPE = "application/vnd.microbrowser.rgb565"
_SUPPORTED_IMAGE_TYPES = {"image/jpeg", "image/png"}
_SUPPORTED_HTML_TYPES = {"", "text/html", "application/xhtml+xml"}
_RED_565 = tuple((value >> 3) << 11 for value in range(256))
_GREEN_565 = tuple((value >> 2) << 5 for value in range(256))
_BLUE_565 = tuple(value >> 3 for value in range(256))


def pillow_available() -> bool:
    return Image is not None


def _require_pillow() -> None:
    if Image is None:
        raise GatewayError(
            "asset_unavailable", "Image conversion requires the Pillow package.", 503
        )


@dataclass(frozen=True, slots=True)
class RGB565Strip:
    width: int
    height: int
    total_height: int
    y: int
    body: bytes


def _validate_source_image(source: PILImage) -> None:
    if source.format not in {"JPEG", "PNG"}:
        raise GatewayError("unsupported_image", "Only JPEG and PNG images are supported.", 415)
    width, height = source.size
    if (
        width <= 0
        or height <= 0
        or width > MAX_SOURCE_DIMENSION
        or height > MAX_SOURCE_DIMENSION
        or width * height > MAX_SOURCE_PIXELS
    ):
        raise GatewayError(
            "image_too_large",
            "Source image dimensions exceed the Reader's 20-megapixel or dimension limit.",
            413,
        )


def _fit_to_width(source: PILImage, requested_width: int) -> PILImage:
    width, height = source.size
    output_width = min(requested_width, width)
    output_height = max(1, round(height * output_width / width))
    if output_height > MAX_OUTPUT_HEIGHT:
        raise GatewayError(
            "image_too_large", f"Scaled image height exceeds {MAX_OUTPUT_HEIGHT} pixels.", 413
        )
    if output_width == width and output_height == height:
        return source.copy()
    return source.resize((output_width, output_height), Image.Resampling.LANCZOS)


def _rgb565_bytes(image: PILImage) -> bytes:
    rgb = image.convert("RGB").tobytes()
    output = bytearray(len(rgb) // 3 * 2)
    destination = 0
    for source in range(0, len(rgb), 3):
        value = _RED_565[rgb[source]] | _GREEN_565[rgb[source + 1]] | _BLUE_565[rgb[source + 2]]
        output[destination] = value & 0xFF
        output[destination + 1] = value >> 8
        destination += 2
    return bytes(output)


def render_rgb565(body: bytes, width: int, y: int, height: int | None) -> RGB565Strip:
    """Decode a JPEG/PNG, fit its width, and return one scaled-output strip."""
    _require_pillow()
    assert Image is not None
    try:
        with Image.open(io.BytesIO(body)) as source:
            _validate_source_image(source)
            source.load()
            fitted = _fit_to_width(source, width)
    except GatewayError:
        raise
    except Exception as error:
        raise GatewayError("invalid_image", "Image data could not be decoded.", 422) from error

    try:
        total_height = fitted.height
        if y >= total_height:
            raise GatewayError(
                "invalid_segment", "Requested y is beyond the scaled image height.", 416
            )
        strip_height = min(height or MAX_STRIP_HEIGHT, MAX_STRIP_HEIGHT, total_height - y)
        strip = fitted.crop((0, y, fitted.width, y + strip_height)).convert("RGB")
        converted = _rgb565_bytes(strip)
        return RGB565Strip(fitted.width, strip.height, total_height, y, converted)
    finally:
        fitted.close()


def _single_query_value(
    query: dict[str, list[str]], name: str, *, required: bool = True
) -> str | None:
    values = query.get(name)
    if not values:
        if required:
            raise GatewayError("invalid_request", f"Missing {name} query parameter.", 400)
        return None
    if len(values) != 1 or not values[0]:
        raise GatewayError("invalid_request", f"Exactly one nonempty {name} is required.", 400)
    return values[0]


def _integer_query_value(
    query: dict[str, list[str]], name: str, default: int | None, minimum: int, maximum: int
) -> int | None:
    value = _single_query_value(query, name, required=False)
    if value is None:
        return default
    try:
        number = int(value)
    except ValueError as error:
        raise GatewayError("invalid_request", f"{name} must be an integer.", 400) from error
    if not minimum <= number <= maximum:
        raise GatewayError(
            "invalid_request", f"{name} must be between {minimum} and {maximum}.", 400
        )
    return number


class GatewayHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address: tuple[str, int], allow_private_hosts: bool) -> None:
        super().__init__(address, GatewayHandler)
        self.url_policy = URLPolicy(allow_private_hosts=allow_private_hosts)
        self._interactive: InteractiveService | None = None
        self._interactive_lock = threading.Lock()

    def navigate_interactive(self, url: str, consent: bool, session: str | None) -> JsonObject:
        if consent is not True:
            raise GatewayError(
                "consent_required", "Interactive mode requires consent set to true.", 403
            )
        URLPolicy().validate(url)
        with self._interactive_lock:
            if self._interactive is None:
                self._interactive = InteractiveService()
            service = self._interactive
        return service.navigate(url, consent, session)

    def server_close(self) -> None:
        super().server_close()
        with self._interactive_lock:
            service = self._interactive
            self._interactive = None
        if service is not None:
            service.close()


class GatewayHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "MicrobrowserGateway/1"
    sys_version = ""
    server: GatewayHTTPServer

    def do_GET(self) -> None:
        self._dispatch(self._get)

    def do_POST(self) -> None:
        self._dispatch(self._post)

    def do_OPTIONS(self) -> None:
        self._method_not_allowed()

    def do_PUT(self) -> None:
        self._method_not_allowed()

    def do_PATCH(self) -> None:
        self._method_not_allowed()

    def do_DELETE(self) -> None:
        self._method_not_allowed()

    def _dispatch(self, action: Callable[[], None]) -> None:
        try:
            action()
        except GatewayError as error:
            self._respond_error(error)
        except Exception as error:
            LOGGER.error("Request failed with %s", type(error).__name__)
            self._respond_error(
                GatewayError("internal_error", "The gateway could not complete the request.", 500)
            )

    def _get(self) -> None:
        path, query = self._request_target()
        if path == "/health":
            self._respond_json(200, {"status": "ok"})
        elif path == "/v1/reader":
            self._reader(query)
        elif path == "/v1/asset":
            self._asset(query)
        else:
            raise GatewayError("not_found", "Endpoint not found.", 404)

    def _post(self) -> None:
        path, _ = self._request_target()
        if path != "/v1/interactive":
            raise GatewayError("not_found", "Endpoint not found.", 404)
        payload = self._read_json()
        url = payload.get("url")
        session = payload.get("session")
        if not isinstance(url, str) or not url:
            raise GatewayError("invalid_request", "url must be a nonempty string.", 400)
        if session is not None and not isinstance(session, str):
            raise GatewayError("invalid_request", "session must be a string when provided.", 400)
        consent = payload.get("consent") is True
        document = self.server.navigate_interactive(url, consent, session)
        self._respond_json(200, document)

    def _reader(self, query: dict[str, list[str]]) -> None:
        url = _single_query_value(query, "url")
        assert url is not None
        resource = fetch(url, self.server.url_policy)
        if resource.content_type not in _SUPPORTED_HTML_TYPES:
            raise GatewayError(
                "unsupported_media", "Reader extraction requires an HTML response.", 415
            )
        self._respond_json(200, extract_html(resource.decode_html(), resource.url))

    def _asset(self, query: dict[str, list[str]]) -> None:
        _require_pillow()
        parse_result = self.server.url_policy
        url = _single_query_value(query, "url")
        width = _integer_query_value(query, "width", MAX_OUTPUT_WIDTH, 1, MAX_OUTPUT_WIDTH)
        y = _integer_query_value(query, "y", 0, 0, MAX_OUTPUT_HEIGHT)
        height = _integer_query_value(query, "height", None, 1, MAX_STRIP_HEIGHT)
        assert url is not None and width is not None and y is not None
        resource = fetch(
            url,
            parse_result,
            max_bytes=MAX_IMAGE_DOWNLOAD_BYTES,
            accept="image/jpeg,image/png",
        )
        if resource.content_type not in _SUPPORTED_IMAGE_TYPES:
            raise GatewayError("unsupported_image", "Upstream must return JPEG or PNG.", 415)
        strip = render_rgb565(resource.body, width, y, height)
        self.send_response(200)
        self.send_header("Content-Type", RGB565_CONTENT_TYPE)
        self.send_header("Content-Length", str(len(strip.body)))
        self.send_header("X-Microbrowser-Width", str(strip.width))
        self.send_header("X-Microbrowser-Height", str(strip.height))
        self.send_header("X-Microbrowser-Total-Height", str(strip.total_height))
        self.send_header("X-Microbrowser-Y", str(strip.y))
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        self.wfile.write(strip.body)

    def _read_json(self) -> dict[str, object]:
        content_type = self.headers.get_content_type()
        if content_type != "application/json":
            raise GatewayError("invalid_json", "Content-Type must be application/json.", 415)
        raw_length = self.headers.get("Content-Length")
        if raw_length is None:
            raise GatewayError("invalid_json", "Content-Length is required.", 411)
        try:
            length = int(raw_length)
        except ValueError as error:
            raise GatewayError("invalid_json", "Content-Length is invalid.", 400) from error
        if length <= 0 or length > MAX_REQUEST_JSON_BYTES:
            self.close_connection = True
            raise GatewayError("invalid_json", "JSON body must be 1 to 16384 bytes.", 413)
        body = self.rfile.read(length)
        try:
            value = json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise GatewayError("invalid_json", "Request body must be valid UTF-8 JSON.", 400) from error
        if not isinstance(value, dict):
            raise GatewayError("invalid_json", "JSON request body must be an object.", 400)
        return value

    def _request_target(self) -> tuple[str, dict[str, list[str]]]:
        try:
            target = urllib.parse.urlsplit(self.path)
            query = urllib.parse.parse_qs(
                target.query, keep_blank_values=True, max_num_fields=16
            )
        except ValueError as error:
            raise GatewayError("invalid_request", "Request target is invalid.", 400) from error
        return target.path, query

    def _method_not_allowed(self) -> None:
        self.send_response(405)
        self.send_header("Allow", "GET, POST")
        self._respond_json(405, {
            "error": {"code": "method_not_allowed", "message": "HTTP method is not allowed."}
        }, send_status=False)

    def _respond_error(self, error: GatewayError) -> None:
        self._respond_json(error.status, {
            "error": {"code": error.code, "message": error.message}
        })

    def _respond_json(
        self, status: int, value: JsonObject, *, send_status: bool = True
    ) -> None:
        body = json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        if send_status:
            self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        self.wfile.write(body)

    def log_request(self, code: int | str = "-", size: int | str = "-") -> None:
        path = urllib.parse.urlsplit(self.path).path
        LOGGER.info("%s %s %s", self.command, path, code)

    def log_error(self, format: str, *args: object) -> None:
        LOGGER.warning("Malformed request from %s", self.client_address[0])


def create_server(
    host: str = "127.0.0.1", port: int = 8765, allow_private_hosts: bool = False
) -> GatewayHTTPServer:
    return GatewayHTTPServer((host, port), allow_private_hosts)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Microbrowser trusted local gateway")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--allow-private-hosts", action="store_true")
    arguments = parser.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")
    with create_server(arguments.host, arguments.port, arguments.allow_private_hosts) as server:
        LOGGER.info("Listening on %s:%d", arguments.host, server.server_address[1])
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            LOGGER.info("Stopping gateway")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
