<div align="center">

# Microbrowser

**Interactive apps and bounded web reading for ESP32-S3.**<br>
Write native applications in markup and a small typed language, or read public web pages through a trusted local gateway.<br>
Run on desktop, in full ESP32-S3 QEMU, or on your own panel—without V8 or a full browser DOM on the device.

![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![ESP-IDF 6.1](https://img.shields.io/badge/ESP--IDF-6.1-E7352C?logo=espressif&logoColor=white)
![Target ESP32-S3](https://img.shields.io/badge/target-ESP32--S3-E7352C)
![Python 3 stdlib compiler](https://img.shields.io/badge/compiler-Python%203%20stdlib-3776AB?logo=python&logoColor=white)
![Display RGB565](https://img.shields.io/badge/display-RGB565-6E40C9)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

<img src="docs/screenshots/demo.gif" width="480" alt="Animated replay of the climate dashboard: target changes, page navigation, live chart, text input and touch scrolling">

<sub>Real framebuffer output from <code>build/microbrowser --headless</code> replaying scripted touch input; nothing is mocked up.<br>
Regenerate it with <code>tools/capture_screenshots.py</code>.</sub>

[Quick start](#quick-start) ·
[Open a website](#open-a-website) ·
[Hello, counter](#hello-counter) ·
[Features](#features) ·
[Architecture](#architecture) ·
[ESP32-S3 &amp; QEMU](#esp32-s3-and-qemu) ·
[Authoring guide](docs/authoring.md) ·
[Package format](docs/package-format.md)
[Hardware publication and portability guide](docs/hardware-api.md)<br>
[Hardware platform plan](docs/hardware-platform-plan.md)
</div>

---

## Why Microbrowser?

Modern browser engines need hundreds of megabytes. A microcontroller has a few hundred kilobytes of SRAM and a few megabytes of PSRAM. Microbrowser keeps the parts of the web model that make UIs pleasant to build—pages, links, history, classes, bindings, event handlers and animations—and moves everything expensive to the build machine:

| Done on your computer by `mbc` | Done on the device by the runtime |
|---|---|
| Parse markup, styles and script | Load a CRC-checked binary package |
| Type-check script, compile to bytecode | Run bounded bytecode in event handlers |
| Resolve classes into per-node styles | Lay out rows, columns and stacks |
| Decode and resize PNGs into RGB565 | Blit pixels; no image decoder |
| Rasterize Inter into glyph tables | Draw antialiased text; no font engine |
| Validate size and structural limits | Repaint only damaged regions |

The preferred format is an **authored-application browser**. For existing HTML pages, the separate [Reader gateway](gateway/README.md) extracts bounded text and images; Interactive mode executes JavaScript only in the explicitly opted-in gateway browser, never on the ESP32.

## Gallery

<table>
  <tr>
    <td align="center"><img src="docs/screenshots/native-dashboard.png" width="320" alt="Climate dashboard"><br><sub>Dashboard: bindings, buttons, slider and pulse animation</sub></td>
    <td align="center"><img src="docs/screenshots/native-details.png" width="320" alt="Environment trends page"><br><sub>Second page: live chart and history navigation</sub></td>
  </tr>
  <tr>
    <td align="center"><img src="docs/screenshots/native-input.png" width="320" alt="Text input with on-screen keyboard"><br><sub>Text input with the built-in on-screen keyboard</sub></td>
    <td align="center"><img src="docs/screenshots/native-scrolled-settings.png" width="320" alt="Scrolled settings page"><br><sub>Touch-scrolled settings with persisted values</sub></td>
  </tr>
  <tr>
    <td align="center"><img src="docs/screenshots/qemu-dashboard.png" width="320" alt="Dashboard captured from the emulated ESP32-S3 LCD"><br><sub>The same package on an emulated ESP32-S3 virtual LCD</sub></td>
    <td align="center"><img src="docs/screenshots/web-reader.png" width="320" alt="Public web page in Reader on the emulated ESP32-S3"><br><sub>An actual HTTPS page converted to Reader and rendered by QEMU</sub></td>
  </tr>
</table>


## Build on GitHub Actions

[`.github/workflows/build.yml`](.github/workflows/build.yml) is the primary build path. Open **Actions → Build → Run workflow**, or push a source change. Builds use GitHub-hosted Linux runners and official Espressif/Nordic toolchain containers; an ESP-IDF or Nordic SDK installation on your laptop is not required.

| Job | Output artifact |
|---|---|
| Shared application packages | `application-packages`: five compiled `.mbp` applications, including the dashboard and GATT conformance application |
| Desktop application and host tests | `microbrowser-desktop`: Ubuntu application and replay captures |
| ESP32-S3 firmware and QEMU dashboard | `microbrowser-esp32s3`: owner-selected GATT firmware, `.mbp` embedded in the image, ESP32 build configuration, and QEMU LCD capture |
| nRF5340 application and network-core firmware | `microbrowser-nrf5340`: application/controller ELF and HEX files, linker maps, and embedded-package identity |

The package job compiles the applications once. Firmware jobs download those exact artifacts, and their build-configuration paths select `gatt_conformance.mbp` explicitly. SDK containers, action revisions, and the Nordic SDK manifest are pinned. Artifacts are retained for seven days.

Use the individual job logs to distinguish package compilation, host tests, firmware builds, and package-download failures. A successful CI compilation is not physical-hardware validation. QEMU cannot provide a physical BLE controller; the nRF job compiles real Zephyr firmware, but radio validation requires hardware and a provisioned peer. Host-modeled GPIO/HTTP behavior is not a hardware proof.

The `gh` CLI and repository push permission are the only GitHub tooling expected on a temporary working machine. No compiler or SDK reinstallation is needed there.

## Quick start

Requires a C++20 compiler, GNU Make, Python 3, SDL2 and libcurl (`sdl2-config` and `curl-config` on `PATH`). The compiler uses only the Python standard library.

```sh
# macOS:          brew install sdl2 curl
# Debian/Ubuntu:  sudo apt install build-essential libsdl2-dev libcurl4-openssl-dev python3

git clone https://github.com/leopck/microbrowser.git
cd microbrowser
make                                                        # builds build/microbrowser
python3 tools/mbc run examples/dashboard.mb -- --size 480x320
```

The mouse acts as a finger: click to press, drag to scroll and click an input to open the keyboard. <kbd>Enter</kbd> submits, <kbd>Esc</kbd> dismisses and <kbd>F1</kbd> toggles layout/render timing.

## Hello, counter

One `.mb` file holds the state, styles, pages and script. This is trimmed from [`examples/counter.mb`](examples/counter.mb):

```xml
<microbrowser name="Counter">
  <state>
    <field name="value" type="f32" value="0"/>
    <field name="status" type="string" value="Ready"/>
  </state>

  <style>
    .card    { background: white; radius: 12; padding: 16; gap: 12; }
    .reading { font-size: 36; font-weight: 600; color: accent; }
    .button  { background: #E9EDF5; color: ink; height: 42; radius: 8; }
  </style>

  <page name="home">
    <column class="card" animate="fade:180">
      <text class="reading" bind="{state.value}" format="%.0f"/>
      <progress bind="{state.value}" min="0" max="100" height="12"/>
      <row>
        <button class="button" text="−" width="grow" on_press="decrease()"/>
        <button class="button" text="+" width="grow" on_press="increase()"/>
      </row>
      <text bind="{state.status}"/>
    </column>
  </page>

  <script><![CDATA[
    fn change_value(delta) {
      state.value = clamp(state.value + delta, 0, 100);
      state.status = "Value: " + string(state.value);
    }
    fn increase() { change_value(1); }
    fn decrease() { change_value(-1); }
  ]]></script>
</microbrowser>
```

```sh
python3 tools/mbc run examples/counter.mb -- --size 480x320   # compile and open
python3 tools/mbc build examples/counter.mb -o build/counter.mbp
python3 tools/mbc inspect build/counter.mbp                    # validate and summarize
```

Assigning to `state.value` repaints only the widgets bound to it. Compile errors report `file:line:column`. The [authoring guide](docs/authoring.md) builds the full two-page version step by step, including settings, navigation and persistence.

## Hardware application quick start

Applications can request named, versioned peripherals while sharing their package
bytes across compatible ports. Permissions and physical pin ownership are
explicit; unsupported roles are not simulated successes.

```sh
python3 tools/mbc build examples/gpio_conformance.mb -o build/gpio_conformance.mbp
python3 tools/mbc inspect build/gpio_conformance.mbp
make check-gpio
```

The host test executes the same artifact through embedded and explicitly
confirmed-download paths. Its GPIO and HTTP responses are modeled. See the
[hardware guide](docs/hardware-api.md) for owner-selected ESP32 and nRF builds,
the capability matrix, and the current physical-validation limits.


## Open a website

Start the trusted gateway in one terminal, then open the desktop browser in another:

```sh
make gateway
```

```sh
# Reader by default: bounded text, links and preconverted RGB565 image strips.
./build/microbrowser --url https://example.com

# Opt in to JavaScript execution inside the gateway's isolated browser context.
./build/microbrowser --url https://example.com --events your-interactive.events
```

An interactive replay contains `interactive https://example.com`, `wait 6000`, then `shot build/interactive.ppm`. The local gateway defaults to `http://127.0.0.1:8765`. Ordinary URL loading never executes website JavaScript on the device and never downloads a native application implicitly. A remote `.mbp` file requires its own explicit Run confirmation; abandon it, and no package is fetched.

Links and previous/next navigation work inside Reader. Responsive checkpoints and image strips are fetched on demand within bounded memory. Reader is not full HTML/CSS compatibility; custom layout and scripts are intentionally excluded. Interactive mode runs JavaScript on the gateway and returns links and extracted content, subject to the [security and feature limits](gateway/README.md).

To publish static reading pages, use `tools/publish_reader.py`; its `.mbr.json` documents and links can be loaded directly over HTTPS without the gateway. External images still require the configured gateway unless the image host returns the native RGB565 strip format. See the [hosting and Reader protocol](gateway/README.md).

## Features

**UI**
- Widgets: `text`, `button`, `input`, `slider`, `progress`, `chart`, `image` and `spacer`
- Layout: `row`, `column` and `stack`; fixed, percentage and `grow` sizing; padding, gap and overlay placement
- Styling: reusable classes, inline attributes, a named palette and `#RRGGBB`, rounded corners and borders
- Antialiased Inter text at 12–36 px in regular and semibold, with word wrap and kerning

**Interaction**
- Touch/pointer input and drag scrolling with a tap/scroll threshold
- Built-in on-screen keyboard, caret editing, `max-length`, `on_change` and `on_submit`
- Multi-page apps with `go("page")`, `back()` and bounded history

**Behaviour**
- Declarative `{state.x}` bindings with per-widget invalidation
- `fade`, `slide` and `pulse` animations evaluated natively each frame without running script
- Typed script: `f32`, `i64`, `bool`, `string`, immutable bytes and opaque resource handles; functions, `if`/`while`, timers, HTTP(S) with JSON paths and persistent storage
- Hard limits on instructions, stack, locals, call depth, timers and in-flight requests

**Tooling**
- `mbc build | inspect | run` compiler CLI
- Headless replay of scripted input with framebuffer screenshots (`--events`, `--screenshot`)
- SPI bandwidth estimation (`--spi-hz`) and a live metrics overlay
- ESP32-S3 firmware with a virtual LCD, an emulated Ethernet network and an automated QEMU self-test

## Architecture

```mermaid
flowchart LR
  subgraph Host["Build machine"]
    SRC[".mb source<br/>markup · style · script"] --> MBC["tools/mbc<br/>parse · type-check · compile"]
    PNG["PNG assets"] --> MBC
    MBC --> MBP[".mbp package<br/>CRC32 sections"]
  end
  subgraph Device["core/ — shared C++20 runtime"]
    MBP --> PKG["package.cpp<br/>load + verify"]
    PKG --> BR["Browser<br/>state · pages · timers"]
    BR --> VM["vm.cpp<br/>bounded bytecode"]
    BR --> LAY["layout.cpp"]
    LAY --> PAINT["paint.cpp<br/>damage regions → RGB565"]
  end
  PAINT --> DESK["platform/desktop.cpp<br/>SDL2 · libcurl"]
  PAINT --> ESP["platform/esp_idf.cpp<br/>ESP-IDF · NVS · async HTTP"]
  PAINT --> ZEPHYR["platform/zephyr_platform.cpp<br/>Zephyr · NVS · bounded HTTP jobs"]

The `Platform` interface ([`include/microbrowser/platform.h`](include/microbrowser/platform.h)) is the porting boundary. It provides the clock, logging, storage and asynchronous HTTP; the rest of `core/` is identical on desktop and device. This diagram shows native applications. Reader content bypasses `mbc`: `core/url_navigation.cpp` fetches `microbrowser-reader-v1` JSON through the gateway or a static `.mbr.json` URL, then `core/json.cpp` validates its fields before native layout and drawing.

**Rendering model.** State writes, animations, caret blinks and presses add regions to a damage list of at most 32 rectangles. Each `tick()` repaints only those regions into the RGB565 framebuffer, and the platform pushes only those regions to the display. Layout runs again only after structural changes such as navigation, scrolling or auto-sized text changes.

## Footprint

Measured with the bundled climate dashboard (3 pages, 141 nodes, 29 functions):

| Metric | Value | Source |
|---|---|---|
| Compiled package | 15,581 bytes | `mbc build` |
| Internal heap free after self-test | 180,455 bytes | QEMU `stats` after dashboard self-test |
| Internal heap low-water mark | 126,904 bytes | same |
| Framebuffer, 480×320 RGB565 | 307,200 bytes in PSRAM | runtime report |
| Main-task stack headroom | 3,568 bytes | same |
| Desktop layout pass | ~0.1 ms | `--headless` report on an Apple-silicon host |

QEMU and desktop timings are not ESP32 performance claims. Measure frame rate on your target panel; on SPI displays, the bus is usually the limit. `--spi-hz` estimates that ceiling.

## ESP32-S3 and QEMU

The firmware in [`firmware/`](firmware/) uses ESP-IDF 6.1 and Espressif's QEMU fork. After installing [ESP-IDF and its QEMU tools](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/tools/qemu.html):

```sh
export IDF_PATH=/path/to/esp-idf
make qemu
```

This builds the dashboard package and firmware, then boots an emulated ESP32-S3 with 8 MiB PSRAM and a 480×320 virtual LCD. Over UART, it drives navigation, scrolling, persistence and the app's self-test. It then saves the LCD as `build/qemu-dashboard.ppm`. The same display path has also been verified with a public HTTPS Reader page using `build/qemu-web-reader.ppm`.

| Command | Purpose |
|---|---|
| `make firmware` | Build only |
| `firmware/run.sh --graphics` | Boot QEMU with a visible display window |
| UART `page`, `stats`, `selftest`, `call NAME` | Inspect or drive a running device |

> **Porting to a real board:** [`firmware/main/main.cpp`](firmware/main/main.cpp) creates the QEMU RGB panel (`esp_lcd_rgb_qemu`). For hardware, replace it with your panel's `esp_lcd` driver and feed touch events to `Browser::pointer_down/move/up`. Layout, drawing, scripts, storage and networking need no changes.

## Nordic GPIO and simulation

`firmware/nrf5340_gpio/build.sh` builds the owner-selected nRF5340 SDK GPIO
application. `firmware/nrf5340_gpio/bsim.sh` executes the same Zephyr driver
against Nordic's input-driven Bsim model: alternating edges, unsubscribe/revocation,
close, and a normal process exit are all required. This does not emulate nRF7002
Wi-Fi or replace physical board validation. `firmware/nrf5340_app/bsim.sh`
builds the shared Browser/portable-artifact target; its headless RGB565 logs
describe the shared renderer, not a physical display.

The Nordic fixture first exercises GPIO1.9 event-link COUNT/CC0 capture with
application TIMER0, verifies stop/restart count preservation and rejects TIMER1,
then closes that route before exercising ordinary GPIO on P0.23. Bsim rejects
other timer instances because its TIMER register-array layout differs from
application MDK; actual hardware validation remains separate.



## Testing

```sh
make test   # compiler unit tests, C++ runtime tests and headless UI replays
make qemu   # embedded build plus emulator interaction and LCD capture
make web-test  # make test, then gateway unit and real-Chromium integration tests
```


```sh
make nrf-gpio-sim  # actual Zephyr driver under Nordic Bsim
make check-gpio   # identical embedded and confirmed-download artifact
```
`make test` replays [`tests/dashboard.events`](tests/dashboard.events) and [`tests/counter.events`](tests/counter.events). These scripts simulate real clicks, drags and typing against the compiled packages and fail on any script error or self-test failure. Write your own with `click`, `down`, `move`, `up`, `text`, `key`, `scroll`, `call`, `wait` and `shot`:

```sh
./build/microbrowser --package build/counter.mbp --headless --size 480x320 \
  --frames 200 --events tests/counter.events --screenshot build/counter.ppm
```

To refresh the desktop screenshots and `demo.gif` from real replays, install Pillow (`pip install -r tools/requirements-fonts.txt`) and run `python3 tools/capture_screenshots.py`. `qemu-dashboard.png` is a conversion of the `build/qemu-dashboard.ppm` written by `make qemu`.

## Scope

Microbrowser does not promise full HTML/CSS/JavaScript compatibility, a browser DOM, selectors or `eval` on the ESP32. Public web pages use the bounded Reader representation; website JavaScript runs only in the explicitly selected gateway browser. These constraints keep native applications small and predictable:

- Every native app is compiled ahead of time from its `.mb` source; downloading a reader document never installs or executes script.
- App script runs only in response to events and timers, under fixed instruction and memory budgets.
- Element IDs are scoped to the current page, and widgets bind only to declared, typed state.
- Website loading is bounded by URL, document, link, text, image and in-flight-request limits.
- The bundled glyph set is fixed; unbundled Unicode characters render as `?`.

The limits and rationale are documented in the [authoring guide](docs/authoring.md), [gateway guide](gateway/README.md) and [package format](docs/package-format.md).

## Repository layout

| Path | Contents |
|---|---|
| [`core/`](core/), [`include/microbrowser/`](include/microbrowser/) | Runtime: package loader, browser, layout, paint, input, VM and fonts |
| [`platform/`](platform/) | Desktop (SDL2/libcurl), headless event replay and ESP-IDF platform implementations |
| [`tools/mbc/`](tools/mbc/) | Compiler and inspector (`python3 tools/mbc`) |
| [`tools/`](tools/) | QEMU smoke test, screenshot capture and font-table generator |
| [`gateway/`](gateway/) | Trusted Reader conversion, RGB565 image extraction and consented Chromium navigation |
| [`examples/`](examples/) | Climate dashboard and counter tutorial apps |
| [`firmware/`](firmware/) | ESP-IDF project for ESP32-S3 |
| [`tests/`](tests/) | Compiler, runtime, font and replay tests |
| [`docs/`](docs/) | [Authoring guide](docs/authoring.md), [package format](docs/package-format.md) and screenshots |
| [`third_party/inter/`](third_party/inter/) | Inter font source and [license notes](third_party/inter/README.md) |

## Contributing

Issues and pull requests are welcome. Before you open a pull request:

1. Push your source change and run the `Build` GitHub Actions workflow. It covers shared packages, native tests, gateway checks, ESP32-S3 QEMU, and nRF5340 firmware compilation.
2. Add or update a replay in `tests/*.events` for user-visible behavior.
3. If you changed the language or binary layout, update [`docs/authoring.md`](docs/authoring.md) or [`docs/package-format.md`](docs/package-format.md).
4. If your change affects UI output, run `tools/capture_screenshots.py`.

Generated font data in `core/font_data.cpp` is produced by `tools/build_fonts.py`; regenerate it rather than editing it by hand. See [`third_party/inter/README.md`](third_party/inter/README.md).

## Acknowledgements

- [Inter](https://github.com/rsms/inter) by Rasmus Andersson, used under the SIL Open Font License 1.1
- [ESP-IDF](https://github.com/espressif/esp-idf) and [Espressif QEMU](https://github.com/espressif/qemu)
- [SDL2](https://www.libsdl.org/) and [libcurl](https://curl.se/libcurl/) for the desktop runtime
- [Playwright](https://playwright.dev/python/) and [Pillow](https://python-pillow.org/) for optional gateway JavaScript execution and image conversion

## License

Microbrowser is released under the [MIT License](LICENSE). The bundled Inter font keeps its own [SIL Open Font License 1.1](third_party/inter/OFL.txt).
