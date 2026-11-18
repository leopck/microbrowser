"""Dependency-free HTML Reader extraction and bounded HTTP fetching."""

from __future__ import annotations

from dataclasses import dataclass, field
from collections.abc import Iterator
from .tcp_proxy import TransportProxy
from html.parser import HTMLParser
import ipaddress
import re
import socket
from typing import cast
from typing import Literal
import urllib.error
import urllib.parse
import urllib.request

MAX_HTML_BYTES = 2 * 1024 * 1024
MAX_BLOCKS = 256
FETCH_TIMEOUT_SECONDS = 15
MAX_TREE_DEPTH = 128
MAX_TREE_NODES = 4096
MAX_BLOCK_TEXT_BYTES = 8192
MAX_TEXT_BYTES = 24_000
MAX_IMAGE_DIMENSION = 16_384
SocketAddress = tuple[str, int] | tuple[str, int, int, int]


def _is_public_address(text_address: str) -> bool:
    address = ipaddress.ip_address(text_address.split("%", 1)[0])
    if isinstance(address, ipaddress.IPv6Address) and address.ipv4_mapped:
        address = address.ipv4_mapped
    return address.is_global and not address.is_multicast

ReaderMode = Literal["reader", "interactive"]
JsonObject = dict[str, object]


class GatewayError(Exception):
    """An error that can be returned in the gateway's JSON error envelope."""

    def __init__(self, code: str, message: str, status: int) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.status = status


@dataclass(frozen=True, slots=True)
class URLPolicy:
    """Validate every upstream destination, including redirect destinations."""

    allow_private_hosts: bool = False

    def _validated_target(self, url: str) -> tuple[str, int]:
        if any(ord(character) < 32 or ord(character) == 127 for character in url):
            raise GatewayError("invalid_url", "URL contains control characters.", 400)
        try:
            parts = urllib.parse.urlsplit(url)
            port = parts.port
            hostname = parts.hostname
        except ValueError as error:
            raise GatewayError("invalid_url", "URL has an invalid host or port.", 400) from error
        if parts.scheme.lower() not in {"http", "https"}:
            raise GatewayError("invalid_url", "Only HTTP and HTTPS URLs are allowed.", 400)
        if not hostname:
            raise GatewayError("invalid_url", "URL must include a host.", 400)
        if parts.username is not None or parts.password is not None:
            raise GatewayError(
                "credentials_blocked", "Credentials in upstream URLs are not allowed.", 400
            )
        if "\\" in parts.netloc:
            raise GatewayError("invalid_url", "URL host contains an invalid character.", 400)
        if parts.scheme.lower() == "https" and not self.allow_private_hosts and port not in {None, 443}:
            raise GatewayError(
                "blocked_origin", "Public HTTPS transport is limited to port 443.", 403
            )
        default_port = 443 if parts.scheme.lower() == "https" else 80
        return hostname.rstrip(".").lower(), port if port is not None else default_port

    def _resolve(self, hostname: str, port: int) -> list[SocketAddress]:
        if not self.allow_private_hosts and (
            hostname == "localhost" or hostname.endswith(".localhost")
        ):
            raise GatewayError("blocked_origin", "Local and private origins are blocked.", 403)
        try:
            ipaddress.ip_address(hostname)
            addresses: list[SocketAddress] = [(hostname, port)]
        except ValueError:
            try:
                resolved = socket.getaddrinfo(hostname, port, type=socket.SOCK_STREAM)
                addresses = [cast(SocketAddress, item[4]) for item in resolved]
            except socket.gaierror as error:
                raise GatewayError("invalid_url", "URL host could not be resolved.", 400) from error
        if not addresses:
            raise GatewayError("invalid_url", "URL host could not be resolved.", 400)
        if not self.allow_private_hosts and any(
            not _is_public_address(address[0]) for address in addresses
        ):
            raise GatewayError("blocked_origin", "Local, private, and non-global origins are blocked.", 403)
        return addresses

    def validate(self, url: str) -> str:
        target = self._validated_target(url)
        if not self.allow_private_hosts:
            self._resolve(*target)
        return url
    def connect(self, url: str) -> socket.socket:
        """Connect only to a validated numeric address; retain the URL hostname for TLS."""
        last_error: OSError | None = None
        for address in self._resolve(*self._validated_target(url)):
            try:
                connection = socket.create_connection(address, timeout=FETCH_TIMEOUT_SECONDS)
            except OSError as error:
                last_error = error
                continue
            try:
                peer = connection.getpeername()[0]
                if not self.allow_private_hosts and not _is_public_address(peer):
                    raise GatewayError(
                        "blocked_origin", "The connected peer is not a permitted public address.", 403
                    )
                return connection
            except Exception:
                connection.close()
                raise
        raise GatewayError(
            "upstream_unavailable", "No validated upstream address accepted a connection.", 502
        ) from last_error


@dataclass(frozen=True, slots=True)
class FetchedResource:
    body: bytes
    url: str
    content_type: str
    charset: str | None

    def decode_html(self) -> str:
        encoding = self.charset or "utf-8-sig"
        try:
            return self.body.decode(encoding, errors="replace")
        except LookupError as error:
            raise GatewayError(
                "unsupported_charset", "Upstream declared an unknown character encoding.", 415
            ) from error


class _PolicyRedirectHandler(urllib.request.HTTPRedirectHandler):
    max_redirections = 5
    max_repeats = 2

    def __init__(self, policy: URLPolicy) -> None:
        super().__init__()
        self.policy = policy

    def redirect_request(  # type: ignore[override]
        self,
        req: urllib.request.Request,
        fp: object,
        code: int,
        msg: str,
        headers: object,
        newurl: str,
    ) -> urllib.request.Request | None:
        destination = urllib.parse.urljoin(req.full_url, newurl)
        self.policy.validate(destination)
        return super().redirect_request(req, fp, code, msg, headers, destination)


class _PinnedProxyHandler(urllib.request.BaseHandler):
    """Direct requests only to the transport created for this fetch."""

    handler_order = 100

    def __init__(self, proxy_url: str) -> None:
        self._proxy_authority = urllib.parse.urlsplit(proxy_url).netloc

    def _proxy_request(self, request: urllib.request.Request, scheme: str) -> None:
        request.set_proxy(self._proxy_authority, scheme)

    def http_open(self, request: urllib.request.Request) -> None:
        self._proxy_request(request, "http")

    def https_open(self, request: urllib.request.Request) -> None:
        self._proxy_request(request, "https")


def fetch(
    url: str,
    policy: URLPolicy,
    *,
    max_bytes: int = MAX_HTML_BYTES,
    accept: str = "text/html,application/xhtml+xml",
) -> FetchedResource:
    """Fetch through a transport whose TCP peers are checked and pinned."""
    policy.validate(url)
    with TransportProxy(policy.connect) as transport:
        return _fetch_through_proxy(
            url, policy, transport=transport, max_bytes=max_bytes, accept=accept
        )


def _fetch_through_proxy(
    url: str,
    policy: URLPolicy,
    *,
    transport: TransportProxy,
    max_bytes: int,
    accept: str,
) -> FetchedResource:
    request = urllib.request.Request(
        url,
        headers={
            "Accept": accept,
            "Accept-Encoding": "identity",
            "User-Agent": "Microbrowser-Reader/1.0",
        },
    )
    opener = urllib.request.build_opener(
        _PolicyRedirectHandler(policy), urllib.request.ProxyHandler({}),
        _PinnedProxyHandler(transport.url),
    )
    try:
        with opener.open(request, timeout=FETCH_TIMEOUT_SECONDS) as response:
            final_url = response.geturl()
            policy.validate(final_url)
            content_length = response.headers.get("Content-Length")
            if content_length and int(content_length) > max_bytes:
                raise GatewayError(
                    "upstream_too_large", "Upstream response exceeds the byte limit.", 413
                )
            body = response.read(max_bytes + 1)
            if len(body) > max_bytes:
                raise GatewayError(
                    "upstream_too_large", "Upstream response exceeds the byte limit.", 413
                )
            raw_content_type = response.headers.get("Content-Type")
            content_type = response.headers.get_content_type() if raw_content_type else ""
            return FetchedResource(
                body=body,
                url=final_url,
                content_type=content_type,
                charset=response.headers.get_content_charset(),
            )
    except GatewayError:
        raise
    except urllib.error.HTTPError as error:
        error.close()
        if error.code in {301, 302, 303, 307, 308}:
            raise GatewayError(
                "redirect_error", "Upstream returned an invalid or excessive redirect.", 502
            ) from error
        raise GatewayError(
            "upstream_http", f"Upstream returned HTTP {error.code}.", 502
        ) from error
    except TimeoutError as error:
        raise GatewayError("upstream_timeout", "Upstream request timed out.", 504) from error
    except urllib.error.URLError as error:
        if isinstance(error.reason, TimeoutError):
            raise GatewayError("upstream_timeout", "Upstream request timed out.", 504) from error
        raise GatewayError(
            "upstream_unavailable", "Upstream resource could not be fetched.", 502
        ) from error
    except ValueError as error:
        raise GatewayError("invalid_url", "URL has an invalid value.", 400) from error


@dataclass(slots=True)
class _Node:
    tag: str
    attrs: dict[str, str]
    children: list[_Node | str] = field(default_factory=list)
    text: str = ""
    image_count: int = 0


_VOID_TAGS = {
    "area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta",
    "param", "source", "track", "wbr",
}
_SKIP_CONTENT_TAGS = {
    "script", "style", "noscript", "svg", "template", "nav", "header", "footer",
    "aside", "form", "button", "select", "textarea", "canvas", "video", "audio",
}
_INLINE_TAGS = {
    "abbr", "b", "bdi", "bdo", "cite", "code", "em", "font", "i", "kbd", "mark",
    "q", "s", "samp", "small", "span", "strong", "sub", "sup", "time", "u", "var",
}
_TEXT_CONTAINER_TAGS = {"address", "blockquote", "figcaption", "li", "p", "pre"}
_HEADING_TAGS = {"h1", "h2", "h3", "h4", "h5", "h6"}
_MANGA_MARKER = re.compile(
    r"(?:manga|manhwa|manhua|comic|chapter[-_ ]?(?:images?|reader)|reading[-_ ]?content)",
    re.IGNORECASE,
)


class _TreeBuilder(HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.root = _Node("#document", {})
        self._stack = [self.root]
        self._node_count = 1

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        self._node_count += 1
        if self._node_count > MAX_TREE_NODES or len(self._stack) > MAX_TREE_DEPTH:
            raise GatewayError(
                "document_too_complex", "HTML exceeds the Reader tree-size or nesting limit.", 422
            )
        node = _Node(tag.lower(), {name.lower(): value or "" for name, value in attrs})
        self._stack[-1].children.append(node)
        if node.tag not in _VOID_TAGS:
            self._stack.append(node)

    def _finish(self, node: _Node) -> None:
        node.image_count = int(node.tag == "img") + sum(
            child.image_count for child in node.children if isinstance(child, _Node)
        )
        if _is_hidden(node):
            node.text = ""
        elif node.tag == "br":
            node.text = " "
        else:
            node.text = "".join(
                child if isinstance(child, str) else child.text for child in node.children
            )

    def handle_startendtag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        self.handle_starttag(tag, attrs)
        node = self._stack[-1].children[-1]
        assert isinstance(node, _Node)
        if node.tag not in _VOID_TAGS:
            self._stack.pop()
        self._finish(node)

    def handle_endtag(self, tag: str) -> None:
        lowered = tag.lower()
        for index in range(len(self._stack) - 1, 0, -1):
            if self._stack[index].tag == lowered:
                while len(self._stack) > index:
                    self._finish(self._stack.pop())
                return

    def handle_data(self, data: str) -> None:
        self._stack[-1].children.append(data)

    def close(self) -> None:
        super().close()
        while self._stack:
            self._finish(self._stack.pop())


def _normalize_text(parts: list[str] | str) -> str:
    text = parts if isinstance(parts, str) else "".join(parts)
    return " ".join(text.split())


def _is_hidden(node: _Node) -> bool:
    if node.tag in _SKIP_CONTENT_TAGS:
        return True
    if "hidden" in node.attrs or node.attrs.get("aria-hidden", "").lower() == "true":
        return True
    return node.attrs.get("role", "").lower() in {"navigation", "banner", "contentinfo"}


def _text(node: _Node | str) -> str:
    return node if isinstance(node, str) else node.text


def _walk_nodes(node: _Node) -> Iterator[_Node]:
    pending = [node]
    while pending:
        current = pending.pop()
        yield current
        pending.extend(child for child in reversed(current.children) if isinstance(child, _Node))


def _content_root(root: _Node) -> _Node:
    candidates = [
        node for node in _walk_nodes(root)
        if node.tag in {"main", "article"}
        and (_normalize_text(_text(node)) or node.image_count > 0)
    ]
    if not candidates:
        return next((node for node in _walk_nodes(root) if node.tag == "body"), root)

    def score(node: _Node) -> tuple[int, int]:
        text_length = len(_normalize_text(_text(node)))
        return text_length + node.image_count * 250, int(node.tag == "main")

    return max(candidates, key=score)


def _absolute_http_url(
    base_url: str, value: str, action_policy: URLPolicy | None = None
) -> str | None:
    value = value.strip()
    if not value:
        return None
    try:
        absolute = urllib.parse.urljoin(base_url, value)
        parts = urllib.parse.urlsplit(absolute)
        if parts.scheme.lower() not in {"http", "https"} or not parts.hostname:
            return None
        if parts.username is not None or parts.password is not None:
            return None
        absolute = urllib.parse.urlunsplit((
            parts.scheme,
            parts.netloc,
            urllib.parse.quote(parts.path, safe="/%:@!$&'()*+,;=-._~"),
            urllib.parse.quote(parts.query, safe="/?%:@!$&'()*+,;=-._~"),
            urllib.parse.quote(parts.fragment, safe="/?%:@!$&'()*+,;=-._~"),
        ))
        if action_policy is not None:
            action_policy.validate(absolute)
        return absolute
    except (ValueError, GatewayError):
        return None




def _positive_dimension(value: str | None) -> int | None:
    if not value or not value.isdecimal():
        return None
    dimension = int(value)
    return dimension if 0 < dimension <= MAX_IMAGE_DIMENSION else None


def _looks_like_manga(root: _Node, content_root: _Node, source_url: str) -> bool:
    url_parts = urllib.parse.urlsplit(source_url)
    if _MANGA_MARKER.search(url_parts.hostname or "") or _MANGA_MARKER.search(url_parts.path):
        return True
    marker = " ".join((content_root.attrs.get("id", ""), content_root.attrs.get("class", "")))
    if _MANGA_MARKER.search(marker):
        return True
    return content_root.image_count >= 3 and len(_normalize_text(_text(content_root))) < 500


class _Blocks:
    def __init__(
        self, source_url: str, manga: bool, action_policy: URLPolicy | None
    ) -> None:
        self.source_url = source_url
        self.manga = manga
        self.action_policy = action_policy
        self.blocks: list[JsonObject] = []
        self._seen_text: set[str] = set()
        self._seen_images: set[str] = set()
        self._text_bytes = 0
        self._full = False

    def add(self, block: JsonObject) -> None:
        if self._full or len(self.blocks) >= MAX_BLOCKS:
            self._full = True
            return
        text = block.get("text")
        if isinstance(text, str):
            text = _normalize_text(text)
            if not text or text in self._seen_text:
                if block.get("type") != "image":
                    return
                block.pop("text", None)
            else:
                remaining = MAX_TEXT_BYTES - self._text_bytes
                if remaining <= 0:
                    self._full = True
                    return
                limit = min(remaining, MAX_BLOCK_TEXT_BYTES)
                encoded = text.encode("utf-8")
                if len(encoded) > limit:
                    text = encoded[:limit].decode("utf-8", errors="ignore").rstrip()
                    self._full = True
                block["text"] = text
                self._text_bytes += len(text.encode("utf-8"))
                self._seen_text.add(text)
        self.blocks.append(block)

    def image(self, node: _Node) -> None:
        source = node.attrs.get("src") or node.attrs.get("data-src")
        if not source:
            return
        absolute = _absolute_http_url(self.source_url, source)
        if not absolute or absolute in self._seen_images:
            return
        self._seen_images.add(absolute)
        block: JsonObject = {"type": "image"}
        if self.manga:
            block["url"] = "/v1/asset?" + urllib.parse.urlencode({"url": absolute})
            block["source_url"] = absolute
        else:
            block["url"] = absolute
        alt = _normalize_text(node.attrs.get("alt", ""))
        if alt:
            block["text"] = alt
        width = _positive_dimension(node.attrs.get("width"))
        height = _positive_dimension(node.attrs.get("height"))
        if width is not None:
            block["width"] = width
        if height is not None:
            block["height"] = height
        self.add(block)

    def emit(self, node: _Node) -> None:
        if self._full or _is_hidden(node):
            return
        if node.tag in _HEADING_TAGS:
            self.add({"type": "heading", "text": _normalize_text(_text(node))})
            self._descendant_images(node)
        elif node.tag in _TEXT_CONTAINER_TAGS:
            block_type = "text" if node.tag == "pre" else "paragraph"
            self.add({"type": block_type, "text": _normalize_text(_text(node))})
            self._descendant_images(node)
        elif node.tag == "img":
            self.image(node)
        elif node.tag == "a":
            href = _absolute_http_url(
                self.source_url, node.attrs.get("href", ""), self.action_policy
            )
            text = _normalize_text(_text(node))
            if href:
                self.add({"type": "link", "text": text, "url": href})
            self._descendant_images(node)
        else:
            self._container(node)

    def _descendant_images(self, node: _Node) -> None:
        for child in _walk_nodes(node):
            if child is not node and child.tag == "img" and not _is_hidden(child):
                self.image(child)

    def _container(self, node: _Node) -> None:
        inline: list[str] = []

        def flush() -> None:
            text = _normalize_text(inline)
            inline.clear()
            if text:
                self.add({"type": "text", "text": text})

        for child in node.children:
            if self._full:
                return
            if isinstance(child, str):
                inline.append(child)
            elif _is_hidden(child):
                continue
            elif child.tag in _INLINE_TAGS:
                inline.append(_text(child))
            elif child.tag == "br":
                inline.append(" ")
            else:
                flush()
                self.emit(child)
        flush()


def _page_title(root: _Node, content: _Node, source_url: str) -> str:
    title_node = next((node for node in _walk_nodes(root) if node.tag == "title"), None)
    title = _normalize_text(_text(title_node)) if title_node else ""
    if not title:
        heading = next((node for node in _walk_nodes(content) if node.tag in _HEADING_TAGS), None)
        title = _normalize_text(_text(heading)) if heading else source_url
    title_bytes = title.encode("utf-8")
    return title_bytes[:512].decode("utf-8", errors="ignore")


def _pagination(
    root: _Node, source_url: str, relation: str, action_policy: URLPolicy | None = None
) -> str | None:
    for node in _walk_nodes(root):
        if node.tag not in {"a", "link"}:
            continue
        relations = set(node.attrs.get("rel", "").lower().split())
        if relation in relations:
            make_url = _absolute_http_url
            url = make_url(source_url, node.attrs.get("href", ""), action_policy)
            if url:
                return url
    return None


def _scripts_present(root: _Node) -> bool:
    return any(node.tag == "script" for node in _walk_nodes(root))


def _extract(
    html: str, source_url: str, mode: ReaderMode, action_policy: URLPolicy | None
) -> tuple[JsonObject, list[str]]:
    parser = _TreeBuilder()
    parser.feed(html)
    parser.close()
    root = parser.root
    content = _content_root(root)
    manga = _looks_like_manga(root, content, source_url)
    actions = (action_policy or URLPolicy()) if mode == "interactive" else None
    builder = _Blocks(source_url, manga, actions)
    builder.emit(content)
    useful = any(
        block["type"] in {"heading", "paragraph", "text", "image"}
        for block in builder.blocks
    )
    interactive_available = mode == "reader" and not useful and _scripts_present(root)
    if interactive_available:
        builder.add({
            "type": "paragraph",
            "text": "This page requires interactive mode. Enable it in the trusted Microbrowser gateway to continue.",
        })
    document: JsonObject = {
        "format": "microbrowser-reader-v1",
        "kind": "unsupported" if interactive_available else ("manga" if manga else "article"),
        "mode": mode,
        "title": _page_title(root, content, source_url),
        "source_url": source_url,
        "previous_url": _pagination(root, source_url, "prev", actions),
        "next_url": _pagination(root, source_url, "next", actions),
        "interactive_available": interactive_available,
        "session": None,
        "blocks": builder.blocks,
    }
    if interactive_available:
        document["unsupported_reason"] = "The page has scripts but no readable static text or images."
    anchors = [
        absolute
        for node in _walk_nodes(root)
        if node.tag == "a"
        for href in [node.attrs.get("href", "")]
        if (absolute := _absolute_http_url(source_url, href, actions)) is not None
    ]
    return document, anchors


def extract_html(
    html: str,
    source_url: str,
    mode: ReaderMode = "reader",
    *,
    action_policy: URLPolicy | None = None,
) -> JsonObject:
    """Convert an HTML string to a microbrowser-reader-v1 document."""
    document, _ = _extract(html, source_url, mode, action_policy)
    return document


def anchor_urls(
    html: str, source_url: str, *, action_policy: URLPolicy | None = None
) -> list[str]:
    """Return absolute HTTP(S) destinations that came only from HTML anchors."""
    _, anchors = _extract(html, source_url, "interactive", action_policy)
    return anchors
