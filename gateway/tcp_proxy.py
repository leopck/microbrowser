"""Loopback-only HTTP transport over connector-validated upstream sockets."""

from __future__ import annotations

from collections.abc import Callable, Iterator
from contextlib import contextmanager
from dataclasses import dataclass
from http import HTTPStatus
import socket
import socketserver
import threading
import time
from types import TracebackType
import urllib.parse

_MAX_LINE_BYTES = 16 * 1024
_MAX_HEADERS_BYTES = 64 * 1024
_MAX_HEADERS = 100
_MAX_CONNECTIONS = 64
_IDLE_TIMEOUT_SECONDS = 15.0
_RELAY_CHUNK_BYTES = 64 * 1024
_REJECT_TIMEOUT_SECONDS = 1.0
_LOOPBACK_ADDRESS = ("127.0.0.1", 0)
_FORWARDED_HEADERS = frozenset(
    {
        "accept",
        "accept-charset",
        "accept-encoding",
        "accept-language",
        "authorization",
        "cache-control",
        "cookie",
        "from",
        "if-match",
        "if-modified-since",
        "if-none-match",
        "if-range",
        "if-unmodified-since",
        "pragma",
        "range",
        "referer",
        "user-agent",
    }
)
_TOKEN_DELIMITERS = frozenset("()<>@,;:\\\"/[]?={} \t")


class _HTTPFailure(Exception):
    def __init__(self, status: int) -> None:
        super().__init__(status)
        self.status = status


class _ProxyStopping(Exception):
    pass


@dataclass(frozen=True, slots=True)
class _Request:
    method: str
    target: str
    version: str
    headers: tuple[tuple[str, str], ...]


@dataclass(frozen=True, slots=True)
class _HTTPDestination:
    url: str
    authority: str
    origin_form: str
    hostname: str
    port: int


def _error_response(status: int, *, include_body: bool = True) -> bytes:
    phrase = HTTPStatus(status).phrase
    body = f"{status} {phrase}\n".encode("ascii") if include_body else b""
    head = (
        f"HTTP/1.1 {status} {phrase}\r\n"
        "Connection: close\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        f"Content-Length: {len(body)}\r\n"
        "\r\n"
    ).encode("ascii")
    return head + body


class _SocketReader:
    """Buffered bounded line reads without hiding bytes intended for a tunnel."""

    def __init__(self, sock: socket.socket) -> None:
        self._socket = sock
        self._buffer = bytearray()

    def readline(self) -> bytes:
        while True:
            newline = self._buffer.find(b"\n")
            if newline >= 0:
                if newline + 1 > _MAX_LINE_BYTES:
                    raise _HTTPFailure(431)
                line = bytes(self._buffer[: newline + 1])
                del self._buffer[: newline + 1]
                return line
            if len(self._buffer) > _MAX_LINE_BYTES:
                raise _HTTPFailure(431)
            remaining = _MAX_LINE_BYTES + 1 - len(self._buffer)
            chunk = self._socket.recv(min(4096, remaining))
            if not chunk:
                if self._buffer:
                    raise _HTTPFailure(400)
                return b""
            self._buffer.extend(chunk)

    def take_buffered(self) -> bytes:
        buffered = bytes(self._buffer)
        self._buffer.clear()
        return buffered


def _request_line(reader: _SocketReader) -> tuple[str, str, str] | None:
    raw_line = reader.readline()
    if not raw_line:
        return None
    if not raw_line.endswith(b"\r\n"):
        raise _HTTPFailure(400)
    fields = raw_line[:-2].split(b" ")
    if len(fields) != 3 or not all(fields):
        raise _HTTPFailure(400)
    raw_method, raw_target, raw_version = fields
    if any(byte < 0x21 or byte > 0x7E for byte in raw_target):
        raise _HTTPFailure(400)
    try:
        method = raw_method.decode("ascii")
        target = raw_target.decode("ascii")
        version = raw_version.decode("ascii")
    except UnicodeDecodeError as error:
        raise _HTTPFailure(400) from error
    if version not in {"HTTP/1.0", "HTTP/1.1"}:
        raise _HTTPFailure(505)
    if not method.isascii() or any(
        not character.isalpha() and character not in "!#$%&'*+-.^_`|~"
        for character in method
    ):
        raise _HTTPFailure(400)
    return method, target, version


def _headers(reader: _SocketReader) -> tuple[tuple[str, str], ...]:
    headers: list[tuple[str, str]] = []
    total_bytes = 0
    while True:
        raw_line = reader.readline()
        if not raw_line or not raw_line.endswith(b"\r\n"):
            raise _HTTPFailure(400)
        total_bytes += len(raw_line)
        if total_bytes > _MAX_HEADERS_BYTES:
            raise _HTTPFailure(431)
        line = raw_line[:-2]
        if not line:
            return tuple(headers)
        if len(headers) >= _MAX_HEADERS or line[:1] in {b" ", b"\t"}:
            raise _HTTPFailure(431 if len(headers) >= _MAX_HEADERS else 400)
        if b":" not in line:
            raise _HTTPFailure(400)
        raw_name, raw_value = line.split(b":", 1)
        value = raw_value.strip(b" \t")
        if any(byte < 32 and byte != 9 or byte == 127 for byte in value):
            raise _HTTPFailure(400)
        try:
            name = raw_name.decode("ascii").lower()
            decoded_value = value.decode("latin-1")
        except UnicodeDecodeError as error:
            raise _HTTPFailure(400) from error
        if not name or any(
            character in _TOKEN_DELIMITERS
            or ord(character) < 33
            or ord(character) > 126
            for character in name
        ):
            raise _HTTPFailure(400)
        headers.append((name, decoded_value))


def _read_request(reader: _SocketReader) -> _Request | None:
    request_line = _request_line(reader)
    if request_line is None:
        return None
    method, target, version = request_line
    return _Request(method, target, version, _headers(reader))


def _url_parts(authority_or_url: str, *, prefix: str = "") -> urllib.parse.SplitResult:
    if any(ord(character) < 32 or ord(character) == 127 for character in authority_or_url):
        raise _HTTPFailure(400)
    try:
        parts = urllib.parse.urlsplit(prefix + authority_or_url)
        hostname = parts.hostname
        port = parts.port
    except ValueError as error:
        raise _HTTPFailure(400) from error
    if (
        not hostname
        or parts.username is not None
        or parts.password is not None
        or "\\" in parts.netloc
        or parts.netloc.endswith(":")
        or port is not None and not 1 <= port <= 65535
    ):
        raise _HTTPFailure(400)
    if ":" not in hostname and any(
        not (character.isascii() and (character.isalnum() or character in ".-_"))
        for character in hostname
    ):
        raise _HTTPFailure(400)
    return parts


def _normalized_hostname(parts: urllib.parse.SplitResult) -> str:
    hostname = parts.hostname
    if hostname is None:
        raise _HTTPFailure(400)
    return hostname.rstrip(".").lower()


def _http_destination(target: str) -> _HTTPDestination:
    if "#" in target:
        raise _HTTPFailure(400)
    parts = _url_parts(target)
    if parts.scheme.lower() != "http" or not parts.netloc:
        raise _HTTPFailure(400)
    port = parts.port or 80
    origin_form = parts.path or "/"
    if "?" in target:
        origin_form += "?" + parts.query
    return _HTTPDestination(
        url=target,
        authority=parts.netloc,
        origin_form=origin_form,
        hostname=_normalized_hostname(parts),
        port=port,
    )


def _connect_url(target: str) -> str:
    parts = _url_parts(target, prefix="https://")
    if (
        parts.path
        or parts.query
        or parts.fragment
        or parts.netloc != target
        or parts.port is None
    ):
        raise _HTTPFailure(400)
    return "https://" + target


def _header_values(headers: tuple[tuple[str, str], ...], name: str) -> list[str]:
    return [value for header, value in headers if header == name]


def _validate_host_header(request: _Request, destination: _HTTPDestination) -> None:
    hosts = _header_values(request.headers, "host")
    if len(hosts) > 1 or request.version == "HTTP/1.1" and not hosts:
        raise _HTTPFailure(400)
    if not hosts:
        return
    host = _url_parts(hosts[0], prefix="http://")
    if (
        host.path
        or host.query
        or host.fragment
        or host.netloc != hosts[0]
        or _normalized_hostname(host) != destination.hostname
        or (host.port or 80) != destination.port
    ):
        raise _HTTPFailure(400)


def _connection_options(headers: tuple[tuple[str, str], ...]) -> set[str]:
    options: set[str] = set()
    for name, value in headers:
        if name in {"connection", "proxy-connection"}:
            options.update(token.strip().lower() for token in value.split(",") if token.strip())
    return options


def _upstream_request(request: _Request, destination: _HTTPDestination) -> bytes:
    connection_options = _connection_options(request.headers)
    lines = [
        f"{request.method} {destination.origin_form} HTTP/1.1\r\n",
        f"Host: {destination.authority}\r\n",
    ]
    for name, value in request.headers:
        if name in _FORWARDED_HEADERS and name not in connection_options:
            lines.append(f"{name}: {value}\r\n")
    lines.append("Connection: close\r\n\r\n")
    try:
        return "".join(lines).encode("latin-1")
    except UnicodeEncodeError as error:
        raise _HTTPFailure(400) from error


class _Tunnel:
    def __init__(self, client: socket.socket, upstream: socket.socket) -> None:
        self._sockets = (client, upstream)
        self._lock = threading.Lock()
        self._last_activity = time.monotonic()
        self._stopped = False

    def record_activity(self) -> None:
        with self._lock:
            self._last_activity = time.monotonic()

    def expired(self) -> bool:
        with self._lock:
            return self._stopped or time.monotonic() - self._last_activity >= _IDLE_TIMEOUT_SECONDS

    def stop(self) -> None:
        with self._lock:
            if self._stopped:
                return
            self._stopped = True
        for sock in self._sockets:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


def _shutdown_write(sock: socket.socket) -> None:
    try:
        sock.shutdown(socket.SHUT_WR)
    except OSError:
        pass


def _relay(
    source: socket.socket,
    destination: socket.socket,
    tunnel: _Tunnel,
    initial: bytes = b"",
) -> None:
    try:
        if initial:
            destination.sendall(initial)
            tunnel.record_activity()
        while True:
            try:
                data = source.recv(_RELAY_CHUNK_BYTES)
            except TimeoutError:
                if tunnel.expired():
                    break
                continue
            if not data:
                _shutdown_write(destination)
                return
            destination.sendall(data)
            tunnel.record_activity()
    except OSError:
        pass
    tunnel.stop()


class _ProxyServer(socketserver.ThreadingTCPServer):
    address_family = socket.AF_INET
    daemon_threads = True
    request_queue_size = _MAX_CONNECTIONS

    def __init__(self, connect: Callable[[str], socket.socket]) -> None:
        self._connect = connect
        self._capacity = threading.BoundedSemaphore(_MAX_CONNECTIONS)
        self._active_lock = threading.Lock()
        self._active_sockets: set[socket.socket] = set()
        self._closing = False
        super().__init__(_LOOPBACK_ADDRESS, _ProxyHandler)

    def connect(self, url: str) -> socket.socket:
        return self._connect(url)

    def _track(self, sock: socket.socket) -> bool:
        with self._active_lock:
            if self._closing:
                return False
            self._active_sockets.add(sock)
            return True

    def untrack(self, sock: socket.socket) -> None:
        with self._active_lock:
            self._active_sockets.discard(sock)

    def track_peer(self, sock: socket.socket) -> bool:
        return self._track(sock)

    def process_request(self, request: socket.socket, client_address: tuple[str, int]) -> None:
        if not self._track(request):
            self.shutdown_request(request)
            return
        if not self._capacity.acquire(blocking=False):
            try:
                request.settimeout(_REJECT_TIMEOUT_SECONDS)
                request.sendall(_error_response(503))
            except OSError:
                pass
            finally:
                self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except BaseException:
            self._capacity.release()
            raise

    def process_request_thread(self, request: socket.socket, client_address: tuple[str, int]) -> None:
        try:
            super().process_request_thread(request, client_address)
        finally:
            self._capacity.release()

    def shutdown_request(self, request: socket.socket) -> None:
        self.untrack(request)
        super().shutdown_request(request)

    def close_active_connections(self) -> None:
        with self._active_lock:
            self._closing = True
            sockets = tuple(self._active_sockets)
        for sock in sockets:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            finally:
                sock.close()

    def handle_error(self, request: socket.socket, client_address: tuple[str, int]) -> None:
        """Avoid the default traceback, which can contain request data in messages."""


class _ProxyHandler(socketserver.BaseRequestHandler):
    server: _ProxyServer
    request: socket.socket

    def setup(self) -> None:
        self._response_started = False
        self._method: str | None = None
        self.request.settimeout(_IDLE_TIMEOUT_SECONDS)

    def handle(self) -> None:
        reader = _SocketReader(self.request)
        try:
            request = _read_request(reader)
            if request is None:
                return
            self._method = request.method
            if request.method == "CONNECT":
                self._handle_connect(request.target, reader)
                return
            if request.method not in {"GET", "HEAD"}:
                raise _HTTPFailure(405)
            self._handle_http(request)
        except _HTTPFailure as error:
            self._send_failure(error.status)
        except _ProxyStopping:
            pass
        except TimeoutError:
            self._send_failure(408)
        except OSError:
            pass
        except Exception:
            self._send_failure(500)

    def _send(self, data: bytes) -> None:
        self._response_started = True
        self.request.sendall(data)

    def _send_failure(self, status: int) -> None:
        if self._response_started:
            return
        try:
            self._send(_error_response(status, include_body=self._method != "HEAD"))
        except OSError:
            pass

    @contextmanager
    def _upstream(self, url: str) -> Iterator[socket.socket]:
        try:
            upstream = self.server.connect(url)
        except TimeoutError as error:
            raise _HTTPFailure(504) from error
        except Exception as error:
            raise _HTTPFailure(502) from error
        with upstream:
            if not self.server.track_peer(upstream):
                raise _ProxyStopping
            try:
                upstream.settimeout(_IDLE_TIMEOUT_SECONDS)
                yield upstream
            finally:
                self.server.untrack(upstream)

    def _handle_http(self, request: _Request) -> None:
        destination = _http_destination(request.target)
        _validate_host_header(request, destination)
        upstream_request = _upstream_request(request, destination)
        with self._upstream(destination.url) as upstream:
            self._forward_http(upstream, upstream_request)

    def _forward_http(self, upstream: socket.socket, request: bytes) -> None:
        try:
            upstream.sendall(request)
            while True:
                data = upstream.recv(_RELAY_CHUNK_BYTES)
                if not data:
                    if not self._response_started:
                        raise _HTTPFailure(502)
                    return
                self._send(data)
        except _HTTPFailure:
            raise
        except TimeoutError as error:
            if not self._response_started:
                raise _HTTPFailure(504) from error
        except OSError as error:
            if not self._response_started:
                raise _HTTPFailure(502) from error

    def _handle_connect(self, target: str, reader: _SocketReader) -> None:
        url = _connect_url(target)
        with self._upstream(url) as upstream:
            self._send(b"HTTP/1.1 200 Connection established\r\n\r\n")
            tunnel = _Tunnel(self.request, upstream)
            relay_thread = threading.Thread(
                target=_relay,
                args=(self.request, upstream, tunnel, reader.take_buffered()),
                name="microbrowser-connect-relay",
                daemon=True,
            )
            relay_thread.start()
            try:
                _relay(upstream, self.request, tunnel)
            finally:
                tunnel.stop()
                relay_thread.join()


class TransportProxy:
    """A bounded loopback proxy whose upstream sockets come from one connector."""

    def __init__(self, connect: Callable[[str], socket.socket]) -> None:
        self._connect = connect
        self._state_lock = threading.Lock()
        self._server: _ProxyServer | None = None
        self._thread: threading.Thread | None = None
        self._url: str | None = None

    @property
    def url(self) -> str:
        with self._state_lock:
            if self._url is None:
                raise RuntimeError("Transport proxy is not running.")
            return self._url

    def __enter__(self) -> TransportProxy:
        with self._state_lock:
            if self._server is not None:
                raise RuntimeError("Transport proxy is already running.")
            server = _ProxyServer(self._connect)
            thread = threading.Thread(
                target=server.serve_forever,
                kwargs={"poll_interval": 0.1},
                name="microbrowser-transport-proxy",
                daemon=True,
            )
            try:
                thread.start()
            except BaseException:
                server.server_close()
                raise
            self._server = server
            self._thread = thread
            self._url = f"http://127.0.0.1:{server.server_address[1]}"
            return self

    def __exit__(
        self,
        exception_type: type[BaseException] | None,
        exception: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        with self._state_lock:
            server = self._server
            thread = self._thread
            if server is None or thread is None:
                return
            try:
                server.shutdown()
            finally:
                try:
                    server.close_active_connections()
                finally:
                    server.server_close()
                    thread.join()
                    self._server = None
                    self._thread = None
                    self._url = None
