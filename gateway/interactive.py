"""Opt-in Chromium navigation with isolated, bounded Playwright sessions."""

from __future__ import annotations

from collections import OrderedDict
from concurrent.futures import Future, TimeoutError as FutureTimeoutError
from dataclasses import dataclass
import os
import queue
import re
import secrets
import threading
import time
from typing import TYPE_CHECKING

from .extract import (
    GatewayError,
    JsonObject,
    MAX_HTML_BYTES,
    URLPolicy,
    anchor_urls,
    extract_html,
)
from .tcp_proxy import TransportProxy

if TYPE_CHECKING:
    from playwright.sync_api import Browser, BrowserContext, Download, Page, Playwright, Route, WebSocketRoute

try:
    from playwright.sync_api import sync_playwright as _sync_playwright
except ImportError:
    _sync_playwright = None

MAX_SESSIONS = 8
MAX_NAVIGATIONS_PER_SESSION = 24
SESSION_IDLE_SECONDS = 15 * 60
NAVIGATION_TIMEOUT_SECONDS = 15
NAVIGATION_RESULT_TIMEOUT_SECONDS = 30
DEFAULT_VIEWPORT_WIDTH = 480
DEFAULT_VIEWPORT_HEIGHT = 800
DEFAULT_CHROMIUM_PATH = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
_SESSION_ID = re.compile(r"^[A-Za-z0-9_-]{32}$")
_SENSITIVE_SELECTOR = ",".join(
    (
        'input[type="password" i]',
        'input[autocomplete*="cc-" i]',
        'input[name*="card-number" i]',
        'input[name*="cardnumber" i]',
        'input[id*="card-number" i]',
        'iframe[src*="captcha" i]',
        '[class*="captcha" i]',
        '[id*="captcha" i]',
    )
)
_DISABLE_SENSITIVE_FEATURES = r"""
window.addEventListener('submit', event => event.preventDefault(), true);
const originalGetContext = HTMLCanvasElement.prototype.getContext;
HTMLCanvasElement.prototype.getContext = function(type, ...args) {
  if (String(type).toLowerCase().includes('webgl')) return null;
  return originalGetContext.call(this, type, ...args);
};
Object.defineProperty(window, 'open', {
  value: () => null, writable: false, configurable: false
});
Object.defineProperty(window, 'WebSocket', {
  value: function() {
    throw new DOMException('WebSockets are disabled in Microbrowser interactive mode.', 'SecurityError');
  },
  writable: false,
  configurable: false
});
"""


def playwright_available() -> bool:
    return _sync_playwright is not None


def require_playwright() -> None:
    if _sync_playwright is None:
        raise GatewayError(
            "playwright_unavailable",
            "Interactive mode requires the pinned Playwright package.",
            503,
        )


def _error_with_cause(code: str, message: str, status: int, cause: Exception) -> GatewayError:
    error = GatewayError(code, message, status)
    error.__cause__ = cause
    return error


@dataclass(slots=True)
class _Session:
    context: BrowserContext
    page: Page
    last_used: float
    navigations: int
    allowed_urls: set[str]


@dataclass(frozen=True, slots=True)
class _Navigation:
    url: str
    session_id: str | None
    result: Future[JsonObject]


class InteractiveService:
    """Run all synchronous Playwright calls on one serialized worker thread."""

    def __init__(self, policy: URLPolicy | None = None) -> None:
        self._policy = policy or URLPolicy()
        self._queue: queue.Queue[_Navigation | None] = queue.Queue()
        self._ready: Future[None] = Future()
        self._navigation_lock = threading.Lock()
        self._close_lock = threading.Lock()
        self._closed = False
        self._thread = threading.Thread(
            target=self._run, name="microbrowser-interactive", daemon=True
        )
        self._thread.start()

    def navigate(
        self, url: str, consent: bool, session_id: str | None = None
    ) -> JsonObject:
        if consent is not True:
            raise GatewayError(
                "consent_required", "Interactive mode requires consent set to true.", 403
            )
        if session_id is not None and not _SESSION_ID.fullmatch(session_id):
            raise GatewayError("invalid_session", "Session identifier is invalid.", 400)
        self._policy.validate(url)
        with self._close_lock:
            if self._closed:
                raise GatewayError("interactive_closed", "Interactive service is closed.", 503)
        with self._navigation_lock:
            try:
                self._ready.result(timeout=NAVIGATION_RESULT_TIMEOUT_SECONDS)
            except FutureTimeoutError as error:
                raise GatewayError(
                    "browser_unavailable", "Chromium startup timed out.", 503
                ) from error
            result: Future[JsonObject] = Future()
            self._queue.put(_Navigation(url, session_id, result))
            try:
                return result.result(timeout=NAVIGATION_RESULT_TIMEOUT_SECONDS)
            except FutureTimeoutError as error:
                raise GatewayError(
                    "navigation_timeout", "Interactive navigation timed out.", 504
                ) from error

    def close(self) -> None:
        with self._close_lock:
            if self._closed:
                return
            self._closed = True
            self._queue.put(None)
        self._thread.join(timeout=NAVIGATION_RESULT_TIMEOUT_SECONDS)

    def _run(self) -> None:
        try:
            with TransportProxy(self._policy.connect) as transport:
                self._run_browser(transport.url)
        except OSError as error:
            if self._ready.done():
                return
            self._ready.set_exception(
                _error_with_cause("transport_unavailable", "The local browser transport could not start.", 503, error)
            )

    def _run_browser(self, proxy_url: str) -> None:
        try:
            require_playwright()
            assert _sync_playwright is not None
            playwright: Playwright = _sync_playwright().start()
        except GatewayError as error:
            self._ready.set_exception(error)
            return
        except Exception as error:
            self._ready.set_exception(
                _error_with_cause(
                    "playwright_unavailable", "Playwright could not be started.", 503, error
                )
            )
            return

        browser: Browser | None = None
        sessions: OrderedDict[str, _Session] = OrderedDict()
        try:
            executable = os.environ.get("MICROBROWSER_CHROMIUM_EXECUTABLE") or DEFAULT_CHROMIUM_PATH
            browser = playwright.chromium.launch(
                executable_path=executable,
                headless=True,
                args=["--disable-dev-shm-usage", "--disable-gpu"],
                proxy={"server": proxy_url},
            )
            self._ready.set_result(None)
            while True:
                navigation = self._queue.get()
                if navigation is None:
                    return
                try:
                    document = self._navigate(browser, sessions, navigation)
                except GatewayError as error:
                    navigation.result.set_exception(error)
                except Exception as error:
                    navigation.result.set_exception(
                        _error_with_cause(
                            "interactive_navigation",
                            "Chromium could not render this page.",
                            502,
                            error,
                        )
                    )
                else:
                    navigation.result.set_result(document)
        except Exception as error:
            if not self._ready.done():
                self._ready.set_exception(
                    _error_with_cause(
                        "browser_unavailable", "Installed Chromium could not be started.", 503, error
                    )
                )
        finally:
            for session in sessions.values():
                self._close_session(session)
            if browser is not None:
                browser.close()
            playwright.stop()

    def _navigate(
        self,
        browser: Browser,
        sessions: OrderedDict[str, _Session],
        navigation: _Navigation,
    ) -> JsonObject:
        now = time.monotonic()
        self._expire_sessions(sessions, now)
        session = self._existing_session(sessions, navigation, now)
        is_new = session is None
        if is_new:
            while len(sessions) >= MAX_SESSIONS:
                _, oldest = sessions.popitem(last=False)
                self._close_session(oldest)
            session = self._new_session(browser, now)

        try:
            self._load_page(session.page, navigation.url)
            html = session.page.content()
            if len(html.encode("utf-8")) > MAX_HTML_BYTES:
                raise GatewayError(
                    "upstream_too_large", "Rendered HTML exceeds the 2 MiB limit.", 413
                )
            if session.page.locator(_SENSITIVE_SELECTOR).count():
                raise GatewayError(
                    "sensitive_content",
                    "Password, card-payment, and CAPTCHA pages are not automated.",
                    422,
                )
            final_url = session.page.url
            self._policy.validate(final_url)
            document = extract_html(
                html, final_url, mode="interactive", action_policy=self._policy
            )
            allowed = set(anchor_urls(html, final_url, action_policy=self._policy))
            allowed.update(
                url for url in (document["previous_url"], document["next_url"])
                if isinstance(url, str)
            )
        except Exception:
            if is_new:
                self._close_session(session)
            raise

        session.navigations += 1
        session.last_used = time.monotonic()
        session.allowed_urls = allowed
        if navigation.session_id is None:
            session_id = secrets.token_urlsafe(24)
            sessions[session_id] = session
        else:
            session_id = navigation.session_id
            sessions[session_id] = session
            sessions.move_to_end(session_id)
        document["session"] = session_id
        return document

    def _existing_session(
        self,
        sessions: OrderedDict[str, _Session],
        navigation: _Navigation,
        now: float,
    ) -> _Session | None:
        if navigation.session_id is None:
            return None
        session = sessions.get(navigation.session_id)
        if session is None:
            raise GatewayError("session_not_found", "Interactive session was not found.", 404)
        if navigation.url not in session.allowed_urls:
            raise GatewayError(
                "action_not_allowed", "Only anchors returned by this session may be opened.", 403
            )
        if session.navigations >= MAX_NAVIGATIONS_PER_SESSION:
            del sessions[navigation.session_id]
            self._close_session(session)
            raise GatewayError(
                "session_navigation_limit", "Interactive session navigation limit was reached.", 429
            )
        session.last_used = now
        return session

    def _new_session(self, browser: Browser, now: float) -> _Session:
        context = browser.new_context(
            accept_downloads=False,
            java_script_enabled=True,
            service_workers="block",
            viewport={"width": DEFAULT_VIEWPORT_WIDTH, "height": DEFAULT_VIEWPORT_HEIGHT},
        )
        context.add_init_script(_DISABLE_SENSITIVE_FEATURES)
        context.route("**/*", self._route_request)
        context.route_web_socket("**/*", self._reject_web_socket)
        page = context.new_page()
        context.on("page", self._close_popup)
        page.on("download", self._cancel_download)
        return _Session(
            context=context,
            page=page,
            last_used=now,
            navigations=0,
            allowed_urls=set(),
        )

    def _load_page(self, page: Page, url: str) -> None:
        page.goto(url, wait_until="domcontentloaded", timeout=NAVIGATION_TIMEOUT_SECONDS * 1000)
        page.locator("body").wait_for(state="attached", timeout=5000)
        page.wait_for_timeout(250)

    def _route_request(self, route: Route) -> None:
        request = route.request
        if request.resource_type == "media" or request.method not in {"GET", "HEAD"}:
            route.abort()
            return
        try:
            self._policy.validate(request.url)
        except GatewayError:
            route.abort()
            return
        route.continue_()

    @staticmethod
    def _reject_web_socket(websocket: WebSocketRoute) -> None:
        websocket.close(code=1008, reason="Interactive WebSockets are disabled.")

    @staticmethod
    def _close_popup(page: Page) -> None:
        # Do not wait for unload handlers while dispatching the popup's creation event.
        page.close(run_before_unload=True)

    @staticmethod
    def _cancel_download(download: Download) -> None:
        download.cancel()

    def _expire_sessions(self, sessions: OrderedDict[str, _Session], now: float) -> None:
        expired = [
            session_id
            for session_id, session in sessions.items()
            if now - session.last_used > SESSION_IDLE_SECONDS
        ]
        for session_id in expired:
            session = sessions.pop(session_id)
            self._close_session(session)

    @staticmethod
    def _close_session(session: _Session) -> None:
        session.context.close()
