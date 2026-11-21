# Microbrowser gateway

A trusted local conversion service for public websites. Static Reader requests never execute website JavaScript or launch Chromium. Chromium is used only by an explicitly consented interactive request.

The device receives a bounded Reader document, not a live browser DOM. Website JavaScript and scripts in downloaded native applications are separate capabilities: the former run only in consented gateway Chromium; the latter require the native browser's explicit Run confirmation.

## Run

Requires Python 3.10 or newer. From the project root:

```sh
make gateway
```

The first run creates `.tools/gateway-venv`, installs the pinned Playwright and Pillow dependencies, then starts the trusted service on `127.0.0.1:8765`. Leave this terminal running. In another terminal:

```sh
./build/microbrowser --url https://example.com
./build/microbrowser --url https://example.com --events your-interactive.events
```

`--gateway http://host:port` overrides the desktop default. For firmware, set `MICROBROWSER_GATEWAY_URL` in `BUILD_DEFS`; the supplied QEMU configuration uses `http://10.0.2.2:8765`.

Set `MICROBROWSER_CHROMIUM_EXECUTABLE=/path/to/chrome` if Chrome or Chromium is not in its usual installed location. HTML extraction and URL validation use the standard library. Pillow is needed for image conversion; Playwright is needed only for interactive browsing.

`MICROBROWSER_CHROMIUM_EXECUTABLE` takes precedence when set. Otherwise the gateway looks for installed Playwright-managed Chromium, Chrome/Chromium on `PATH`, then the usual macOS application path. The Linux CI job installs Playwright's matching Chromium before running `make web-test`, so the real-browser test is not skipped for lack of a macOS installation.

Reader and asset requests block non-global IPv4/IPv6 addresses, loopback/local hostnames, URL userinfo and non-HTTP(S) schemes, including every resolved address and redirect destination. `--allow-private-hosts` lifts that restriction only for trusted local Reader and image fixtures. Interactive requests always use the public-origin policy, even when this flag is set.

The listener is trusted and unauthenticated, and emits no permissive CORS headers. Do not expose it to an untrusted network. Chromium isolation is not a hardened sandbox for hostile websites: enabling interactive browsing means explicitly consenting to that page's JavaScript running on the gateway host. The native client verifies gateway TLS by default outside loopback; `--insecure-gateway` is a separate development override.

## HTTP API

All errors use an HTTP 4xx/5xx response and a JSON body:

```json
{"error":{"code":"blocked_origin","message":"Local, private, and non-global origins are blocked."}}
```

### `GET /health`

Returns HTTP 200 and `{"status":"ok"}`.

### `GET /v1/reader?url=<absolute-URL>`

Fetches HTML and returns HTTP 200 with a Reader document:

```text
{
  format: "microbrowser-reader-v1",
  kind: "article" | "manga" | "unsupported",
  mode: "reader" | "interactive",
  title: string,
  source_url: string,
  previous_url: string | null,
  next_url: string | null,
  interactive_available: boolean,
  session: string | null,
  unsupported_reason?: string,
  blocks: Block[]
}
```

Normal Reader responses have `mode:"reader"` and `session:null`. `previous_url`, `next_url` and standalone links are absolute HTTP(S) URLs. Blocks use these forms:

```text
{ type:"heading"|"paragraph"|"text", text:string }
{ type:"link", text:string, url:string }
{ type:"image", text?:string, url:string, source_url?:string, width?:integer, height?:integer }
```

Article images use absolute source URLs. Manga images additionally have `source_url` and a gateway-local `/v1/asset?url=<percent-encoded-source-URL>` URL.

A script-only page with no useful static text/images is not an HTTP error: it returns `kind:"unsupported"`, `interactive_available:true`, `unsupported_reason`, and a paragraph offering interactive mode.

The extractor prefers useful `main`/`article` content, includes inline markup in headings/paragraphs, decodes entities, normalizes whitespace and UTF-8, removes repeated text and images, and skips navigation, forms, hidden elements, scripts, styles, `noscript`, SVG, media and other page chrome.

### `GET /v1/asset?url=<absolute-URL>&width=<pixels>&y=<pixels>&height=<pixels>`

Requires Pillow and an upstream JPEG or PNG. Parameters:

- `url`: required absolute image URL; redirects and private-origin rules match Reader fetching.
- `width`: optional requested viewport content width, default `480`, range `1..480`. Images never upscale.
- `y`: optional zero-based offset in the **scaled-output image**, default `0`.
- `height`: optional maximum output strip height, range `1..640`, default at most `640`; the final strip can be shorter.

The server fits the original image to `width`, then slices the scaled result at `y`/`height`. The response is raw row-major RGB565 little-endian data, exactly `X-Microbrowser-Width * X-Microbrowser-Height * 2` bytes, with:

```text
Content-Type: application/vnd.microbrowser.rgb565
X-Microbrowser-Width: actual strip width
X-Microbrowser-Height: actual strip height
X-Microbrowser-Total-Height: full scaled-output image height
X-Microbrowser-Y: scaled-output strip offset
```

Use `X-Microbrowser-Total-Height` to request the next strip. A missing outside-image `y` returns an error rather than a wrapped strip. The native reader requests visible strips at reserve priority, evicts cached strips beyond its 2 MiB budget, and permits at most four pending image fetches.

### `POST /v1/interactive`

Requires `Content-Type: application/json`:

```json
{"url":"https://example.com","consent":true,"session":"optional-existing-session-id"}
```

Returns the same Reader document with `mode:"interactive"` and an unguessable session string. Pass that string, an absolute URL returned for an anchor or pagination link, and `consent:true` to navigate in the same browser context. Other URLs are rejected for that session; no arbitrary elements are clicked.

Synchronous Playwright calls run on one worker thread. Each session uses a fresh isolated browser context, with at most 8 live sessions, LRU/idle eviction and 24 successful navigations per session. Every navigation requires consent and a validated destination; consent to one document does not authorize arbitrary destinations. Normal Reader requests never create a Chromium context.

### Transport protections

Interactive Chromium uses `TransportProxy`, which validates every browser target and redirect. It resolves the hostname, rejects non-global addresses, then connects to a validated numeric address with a socket timeout. The original hostname is retained for HTTP Host and HTTPS SNI/certificate verification; this is not a certificate-bypassing CONNECT service or an arbitrary TCP tunnel.

HTTP request lines, headers and connection counts are bounded. Supported subrequest methods are restricted. Popup pages and WebSockets are rejected; form submission, non-GET/HEAD subrequests, media, downloads, service workers and WebGL are disabled. Obvious password, card-payment and CAPTCHA pages are refused rather than treating them as ordinary reading content. Interactive mode never accepts private origins.

## Static Reader hosting

To serve Reader documents without running the gateway, convert public pages into snapshots and a linked index:

```sh
python3 tools/publish_reader.py article=https://example.com \
  --output public-reader --site-base https://your-site.example/reader/
```

This uses the same extraction bounds and origin policy, and writes `public-reader/article.mbr.json` plus `public-reader/index.mbr.json`. Upload the directory to your HTTPS host. Gateway-relative manga image URLs are rewritten to their upstream sources.

Open `./build/microbrowser --url https://your-site.example/reader/index.mbr.json`. Reader responses may use `application/json` or `application/vnd.microbrowser.reader+json`, with a correct `Content-Length`. The document, its text and its page links need no live gateway, but image URLs are still sent through the device's configured gateway for RGB565 conversion. Without that service, external JPEG/PNG images are unavailable; a compatible image host may instead return the required RGB565 strip and `X-Microbrowser-*` metadata itself. Static hosting neither executes website JavaScript nor implicitly installs a native application.

## Limits

| Item | Limit |
|---|---:|
| Reader fetch/socket timeout | 15 seconds |
| Interactive navigation / result wait | 15 / 30 seconds |
| Static and rendered HTML | 2 MiB compressed or decompressed |
| HTML tree depth / nodes | 128 / 4,096 |
| Redirects | 5 |
| Reader blocks | 256 |
| Combined Reader block text | 24,000 UTF-8 bytes |
| Reader title | 512 UTF-8 bytes |
| Each block's text | 8,192 UTF-8 bytes |
| Reader URL | 2,047 UTF-8 bytes |
| Image dimension metadata | 16,384 pixels |
| Interactive JSON request | 16 KiB |
| Interactive sessions | 8 |
| Navigations per interactive session | 24 |
| Interactive idle eviction | 15 minutes |
| JPEG/PNG download | 16 MiB |
| Source image | 20,000,000 pixels; each dimension at most 32,768 |
| Scaled image total height | 32,768 pixels |
| Asset strip | width at most 480, height at most 640, bytes at most 614,400 |
| Native cached image budget | 2 MiB |

The native JSON parser checks these fields before allocation. Native URLs reject embedded credentials, unsupported schemes and control characters. Reader fields cannot install or execute application bytecode.

## Supported and excluded behavior

Static extraction supports ordinary article/manga reading content, headings, paragraphs, text, standalone anchors, pagination metadata and images. It is not full web emulation and does not preserve layout or website scripts. Manga classification uses URL/content markers and image-heavy pages. Lazy images using `data-src` are recognized; arbitrary script-based lazy loading requires interactive mode.

Interactive mode renders JavaScript and offers link navigation only. It deliberately does not automate login, forms, passwords, card/payment flows, CAPTCHA, arbitrary clicks, popups, WebSockets, downloads, video/audio or WebGL. Browser storage is confined to the isolated context and destroyed on eviction/shutdown. Complex JavaScript pages may render differently when CSS is excluded, and canvas or streaming content is not faithfully reproduced.

## Tests

```sh
make web-test
```

This runs all native compiler/runtime/replay tests, then gateway unit and integration tests. They cover extraction, malformed and excessive HTML, blocked origins, redirects, exact RGB565 output, DNS-rebinding protection, TLS hostname preservation, consent, session navigation and sensitive-page refusal. Tests create loopback HTTP fixtures and native packages from repository fixtures. Real Chromium tests run when Playwright and an installed Chrome executable are available; otherwise they are reported as skipped.

`.tools/gateway-venv`, browser binaries and generated test artifacts are ignored by Git.
