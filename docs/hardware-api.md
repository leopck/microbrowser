# Hardware applications and portability

This is the guide for **authored hardware applications**, not arbitrary public websites.
Read [package-format.md](package-format.md) for the MBPK container and this page for
hardware publication, permissions, operation semantics, and validation evidence.

## Build once, embed or download those bytes

Compile an application on the host. The device never parses its `.mb` source or
recompiles the website for a particular MCU.

```sh
python3 tools/mbc build examples/gpio_conformance.mb -o build/gpio_conformance.mbp
python3 tools/mbc inspect build/gpio_conformance.mbp
make check-gpio
```

The inspected artifact is MBPK **3.0**, 1,393 bytes, with seven state fields and
nine functions. `make check-gpio` runs those exact bytes twice through `Browser`:
once embedded and once after explicit native-package confirmation and delivery.
The GPIO backend and HTTP response in this test are host models. They are not
proof of physical GPIO, Wi-Fi, TLS, or publisher authentication.

For an ESP32-S3 firmware image:

```sh
firmware/run.sh --build-only --package build/gpio_conformance.mbp
```

The script prints the artifact's SHA-256. `firmware/CMakeLists.txt` copies the
selected file to `firmware/main/application.mbp`; ESP-IDF embeds those same
bytes. There is no alternate board-specific application source. The application
also needs its `user_button` endpoint enabled by the board owner. No physical
GPIO is exposed by default; choose the Kconfig inputs and pin from the actual
board's reserved-pin map.

`MICROBROWSER_PACKAGE=/absolute/path/to/application.mbp` can supply the same
selection through the environment. A relative `--package` path is resolved from
the repository root.

### Downloading instead of embedding

Native URLs ending in `.mbp` have a separate confirmation path. A Reader
or website response is never allowed to acquire hardware permissions merely by
containing bytecode.

- ESP32: an explicitly configured `CONFIG_MB_PACKAGE_URL` first opens the
  package-confirmation page and then uses the same confirmation as its Reader
  button. Empty keeps the embedded application. Validate the URL and trust its
  author before setting it. The asynchronous transport already uses the real
  ESP-IDF HTTP task, not the UI loop.
- nRF: `firmware/nrf5340_app` supports one explicitly enabled package fetch.
  `CONFIG_MB_PACKAGE_FETCH_ENABLED=y`, `CONFIG_MB_PACKAGE_URL`, and an explicit
  CA security tag are required for HTTPS. A failed fetch retains the embedded
  application and reports the error; an application-level retry is not invented.
  Plain HTTP is rejected unless `CONFIG_MB_ALLOW_PLAIN_HTTP=y` is explicitly set.

Owner-provision nRF Wi-Fi separately; no credentials are committed. Zephyr's
Wi-Fi credential storage can provision them at runtime. Alternatively,
`firmware/nrf5340_app/local.conf` is ignored by Git and may hold owner-provided
`CONFIG_WIFI_CREDENTIALS_STATIC_*` values. A DER CA can be supplied with
`-DMICROBROWSER_TLS_CA_CERTIFICATE=/absolute/path/to/ca.der` and the selected
`CONFIG_MB_TLS_CA_SEC_TAG`. Do not use fake public credentials or disable peer
verification for a production deployment.

**CRC32 catches malformation and accidental corruption; it is not a signature.**
HTTPS authenticates a server connection, not the application author. A v3
package has no currently documented publisher-signature/authentication flow.
A deployment that needs publisher identity must add and verify that policy,
rather than treating the CRC, URL hash, or native Run confirmation as proof of
publisher identity.

## Declaring hardware requirements

```xml
<hardware>
  <require capability="gpio_input" version="1.0"/>
  <permission endpoint="user_button"/>
</hardware>
```

The 1.x numeric capability and operation IDs are append-only. `gpio_input` and
`gpio_output` are role selectors for the same GPIO capability; configuration
chooses direction. `PERM` lists permitted logical names; `CAPS` records required
capability versions. Empty or missing permission names deny access. Owner-only
endpoints do not become accessible by naming them in an application's manifest.

`mbc` checks operation arities and types at build time. Before calling a handler,
the runtime checks capability/version, role, permissions, opaque-handle
ownership, operation ownership, buffer limits, state, and backend support.
A missing required endpoint fails before its on-load operation chain starts.
An unsupported physical role is absent, not an empty successful transfer.

Opaque handles contain a capability, slot, application-generation, and
object-generation. They are not pointers, pin numbers, channel addresses, or
integers that a script can forge. A closed/reallocated slot cannot be reopened
by its old handle. Handles are not persistent state and cannot be saved through
`load`/`save`: each launch obtains fresh resources.

## Default budgets and completion semantics

An ordinary browser session has these limits before tighter driver limits:

| Limit | Default |
|---|---:|
| Open handles | 8 |
| Pending operations | 8 |
| Immutable buffer | 4,096 bytes |
| Retained application I/O bytes | 65,536 bytes |

These are device-operation budgets, not a bound on the entire UI or framebuffer.
Submitting work returns after bounded validation/queueing. Completion callbacks
do not block the UI task on physical I/O. Native timing or PCM streaming does
not execute bytecode per hardware sample. The VM's 20,000-instruction event
budget limits script CPU work; it is not wall-clock or electrical timing proof.

Operation callbacks receive:
`(operation_handle, status, primary_result)`.
Notification callbacks receive `(subscription_handle, status, value)`.
The `operation_handle` is not the opened device handle. GPIO subscription
completion returns the subscription handle as its primary result.

| Status | Meaning |
|---:|---|
| 0 | Completed |
| 1 | Invalid argument/relationship |
| 2 | Permission denied |
| 3 | Unsupported operation/role/version |
| 4 | Busy |
| 5 | Timeout |
| 6 | Device lost |
| 7 | I/O failure |
| 8 | Memory exhausted |
| 9 | Cancelled |

A rejected close/unsubscribe does not revoke a valid lease. An accepted close
revokes delivery to the prior generation before the vendor drain completes, so
old callbacks cannot arrive in a replacement application. Accepted control can
revoke a subscription while physical teardown finishes asynchronously.

## Operation and value reference

### Generic calls

- `io.open(capability_id, endpoint, minimum_minor, callback)` obtains a handle.
- `io.configure`, `io.start`, `io.stop`, `io.read`, `io.write`, `io.transfer`,
  `io.subscribe`, `io.unsubscribe`, `io.connect`, `io.disconnect`,
  `io.discover`, `io.advertise`, and `io.notify` take the typed arguments below.
- `io.close(device, callback)` and `io.cancel(operation_handle)` manage lifetime.
- `[...]` in this guide means a logical endpoint or buffer name, not raw native
  storage or a hardware address.

`io.capability_supported(id, endpoint)` tests an available name, not blanket
vendor support. Registering an endpoint also constrains its advertised operation
set. `io.metadata(handle, field [, index])` reads typed result metadata;
field `1` is I/O status, `2` timestamp nanoseconds, `3` sequence, `4` modeled,
and fields `5`/`6` capability major/minor. Additional fields expose actual
clock/format values, transfer counts, PCM buffers, pulse records, BLE
attributes/descriptors, event counts, and USB descriptors. The canonical numeric
field list is `include/microbrowser/device_api.h`; callback and argument
variants are exhaustively represented in `tools/mbc/device_api.py`.

Use `io.metadata(operation, 2i64)` for an operation's timestamp where that
operation reports one. Do not assume that timestamp is an ISR-accurate pulse
measurement: coalesced GPIO levels use the OS/driver clock domain, not a timer
capture unit.

### Numeric and binary values

- `f32`: UI-compatible binary32 value; `i64`: exact signed 64-bit integer.
- `bool`: Boolean; `string`: bounded application text, not binary transfer data.
- `bytes`: immutable binary data, including NUL and non-UTF-8 bytes.
- Empty handle: no resource; it is not a valid device, operation, or subscription.

Helpers `io.bytes()`, `io.bytes(string)`, `io.one_byte(u8)`,
`io.append_byte(bytes, u8)`, `io.concatenate_bytes(a, b)`,
`io.bytes_length(bytes)`, `io.byte_at(bytes, index)`, `io.integer(value)`,
`io.number(value)`, and `io.usb_setup(type, request, value, index, length)`
construct typed data. Buffer-growing helpers copy and are unsuitable for a
per-frame hot loop; create bounded request buffers at setup/state transitions.
Drivers retain immutable inputs until completion or bounded release.

### SPI, I²C, PCM, and GPIO examples

GPIO configuration uses direction `0=input/1=output`, pull
`0=none/1=up/2=down`, then an initial output level. Subscription edges are
`0=rising/1=falling/2=both`. Level events are bounded/coalesced and distinct
from lossless pulse capture.

```text
io.configure(button, 0i64, 1i64, false, "button_configured")
io.subscribe(button, 2i64, "button_subscribed", "button_level")
```

SPI configuration is `frequency_hz`, mode `0..3`, bit order `0=MSB/1=LSB`,
lane count `1/2/4`, and callback. The response reports the selected/actual
frequency available from the driver, not an assumed bypass of clock granularity.
Multilane uses the separately declared capability. Chip select is owner-owned.

```text
io.open(6i64, "spi_sensor", 0i64, "spi_opened")
io.configure(spi, 1000000i64, 0i64, 0i64, 1i64, "spi_configured")
io.transfer(spi, io.one_byte(159i64), 2i64, "spi_transferred")
```

A transfer returns received bytes when present; otherwise its primary result
is the completed count. `io.metadata(...received)` is the actual buffer for a
receive-capable operation. Full-duplex zero-extension of shorter TX differs
from half-duplex phases. Unsupported lane/hardware/address combinations must
be rejected, not approximated. Peripheral-role SPI is a distinct capability;
no controller endpoint may claim peripheral operation without that backend.

I²C uses 7/10-bit logical addresses at the bus topology assigned by the owner.
Its configuration callback receives the selected nominal clock; that is not a
measurement of rise-time-dependent wire speed. Read/write are
`io.read(device,address,ten_bit,max_bytes,callback)` and
`io.write(device,address,ten_bit,bytes,callback)`.
`io.transfer(device,address,ten_bit,write_bytes,read_bytes,repeated_start,callback)`
keeps combined operations within one bounded transaction. Repeated start must
not split a transaction into independent write/read calls. NACK,
arbitration/timeout, write count, actual read bytes, and fault type are results,
not empty success. Ten-bit addresses must not be truncated.

For PCM, configure `frame_rate_hz`, channel count, sample format
(`0=s16 LE`, `1=s24 LE`, `2=s32 LE`, `3=f32 LE`), maximum frames/block and a
callback. For example:

```text
io.configure(audio, 16000i64, 2i64, 0i64, 240i64, "pcm_configured")
```

Read/transfer callbacks return immutable PCM bytes or frame counts as the
operation specifies. Metadata exposes actual frame rate, channels, format,
frame/byte counts, and receive buffers. Reject fractional frames, incompatible
slot/clock configurations, unsupported float conversion, and buffer overruns;
do not drop or repack bytes silently. DMA buffers are bounded and cannot point
to expired application memory. A backend without support for floating-point
samples must explicitly reject `f32 LE`.

### ADC, PWM, timers, and pulse I/O

ADC configuration takes `sample_time_ns` and `reference_microvolts`.
Its response reports actual sample time/reference/resolution/calibration.
`io.read` returns an integer sample, with timestamp/calibration metadata.
A driver does not report calibration merely because a nominal reference value
was supplied.

PWM configuration takes `period_ns` and `initial_active_time_ns`;
`io.write` takes its active duration. Returned actual periods are checked
against hardware timer resolution and owner limits.

Pulse capture configuration takes minimum duration and maximum pulses.
Capture results contain levels, durations and timestamps as bounded records,
including `pulse_count`, `pulse_timestamps_ns_le64`, `pulse_samples_le64` and
`durations_ns_le64` metadata. Pulse output configuration takes resolution and
idle level (`0=low/1=high`); waveform output uses bounded duration sequences and
reports actual transitions. These functions require genuine timer/counter or
hardware-linked resources, not VM-generated timing busy loops.

### UART and RS-485

UART configuration is baud, data bits, stop bits (`1/2`), parity
(`0=none/1=even/2=odd`), hardware flow control (`0/1`), and RS-485 mode
(`0=disabled/1=half duplex`). `io.read(device,maximum_bytes,callback)` returns
bounded bytes; byte writes report consumed count and metadata exposes overflow,
framing, parity and break flags. Claim an RS-485 direction mode only where the
port actually controls it. Shared console/debug output does not imply an
advertised application UART endpoint.

### BLE GATT

`esp_idf_ble_central.*` and `zephyr_ble_central.*` implement real central
clients using NimBLE and Zephyr respectively. They do not return the test
model's fabricated connection, attribute, or notification data. Use capability
15, ABI 1.0, and the owner-registered endpoint name. The supplied physical
profiles register exactly one route, `ble_central`; the application must grant
that name explicitly. `ble_central_model` belongs only to host conformance tests.

```text
<require capability="ble_central" version="1.0"/>
<permission endpoint="ble_central"/>
io.open(15i64, "ble_central", 0i64, "opened")
```

Supported central operations are scan/service/characteristic discovery,
connect/disconnect, read, write, characteristic and connection-state
subscribe/unsubscribe, cancellation, and close. Peripheral-role advertising
and notifications by an application-owned server are not provided by these
the client discovers the current peer's actual service/characteristic handles
and binds every operation to that connection generation. A synchronous Nordic
disconnect failure is not retried continuously by the native worker; a new
disconnect/close, release, or shutdown attempt can retry it.

| Backend storage ceiling | ESP32-S3 NimBLE | nRF5340 Zephyr |
|---|---:|---:|
| Owner connections per route | 1 | 1 |
| Pending operations | 8 | 8 |
| Subscriptions per owner/route | 8 | 8 |
| Retained scan results | 16 | 16 |
| Services / characteristics | 16 / 32 | 16 / 32 |
| Advertisement bytes per record | 255 | 1,650 |
| ATT attribute value bytes | 512 | 512 |

These are implementation ceilings, not guaranteed concurrency under a tighter
application `DeviceBudget` or Bluetooth controller configuration. Logical names
are owner-selected identifiers of at most 63 bytes. Nordic shares one
16-result scan store among its registrations. Its application-thread facade
runs one route per owner and private native operations on an 8 KiB work-queue
stack; it never calls `bt_disable` to disable other owners' Bluetooth use.
The ESP backend may be configured for up to four routes, but the supplied
profile registers `ble_central` and sets `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`.

UUIDs are two- or 16-byte little-endian binary values; 32-bit UUIDs are not
accepted. For example, UUID `ff00` is `io.bytes("00ff")`. Empty UUIDs are
permitted only where the discovery/subscription form specifies a wildcard.
Results contain actual peer addresses, binary payload bytes, RSSI, discovered
UUIDs, characteristic properties, connection state, and written/notification
byte counts. Expected statuses vary with the real peer. The NCS discovery path
uses SDK read callbacks carrying ATT status, rather than treating every null
attribute from the older discovery callback as successful completion; a failed
read does not leave a usable partial service cache.

Read values are copied into bounded retained buffers. Nordic performs real long
reads where applicable, but each write is one request/command and is limited to
`ATT_MTU - 3`; it does not masquerade a long write as an acknowledged complete
write. Subscription setup and teardown use actual CCCD operations. A full
notification queue has bounded loss, not fabricated or partial notification
values. Close cancels application delivery before native cleanup finishes.

The `paired` flag is a native security observation, not a guarantee of a
persisted authenticated bond. Nordic reports observed encryption at security
level L2 or higher; L2 can be unauthenticated encryption without MITM protection
or persistence. ESP32-S3 reads NimBLE's bonded flag. Neither backend initiates
pairing merely to make this flag true. The Nordic `gatt.conf` profile sets
`CONFIG_BT_SMP=n`; do not use this flag as an authentication factor.

#### Owner builds and Bsim fixture

```sh
firmware/run.sh --owner-profile firmware/sdkconfig.gatt_central \
  --build-only --package build/dashboard.mbp
firmware/nrf5340_app/build.sh --gatt nrf7002dk/nrf5340/cpuapp
```

For manual Nordic tool installations, `NCS_CMAKE` overrides the CMake executable used by `build.sh`. Derive it from the selected toolchain environment (for example, `command -v cmake`) when that package does not place CMake in `${NCS_TOOLCHAIN}/bin`. The GitHub workflow supplies this setting from the official Nordic image together with `NCS_TOOLCHAIN`, `NCS_PYTHON` and the release source root shown in `firmware/nrf5340_app/build.sh`.

The ESP fragment selects actual NimBLE/controller support and limits this
profile's scan window to 5 seconds. The Nordic `build.sh --gatt` path keeps the
nRF7002 Wi-Fi transport, merges `gatt.conf`, and builds the generic Zephyr
`hci_ipc` network-core controller. `gatt.conf` reserves a 180,224-byte
application work-heap in addition to the private 8 KiB BLE work-queue stack.
These are intentional budgets to check in the linker report, not a promise of
unbounded application memory or successful board coexistence.

`examples/gatt_conformance.mb` is one authored package for compatible native
ports. It verifies the fixture's real `ff00/ff01` initial value, writes and reads
back four bytes, rejects a write to read-only `ff02`, subscribes, then accepts
exactly one five-byte notification triggered by the peer. It finishes only
after unsubscribe, an observed disconnect notification, connection-subscription
removal, and endpoint close. An ordinary modeled backend cannot satisfy its
required non-modeled native endpoint declaration.

Start with a freshly initialized peripheral for this test. Its initial value is
`00ff80`; after a successful write it retains the new value until reboot,
including across disconnects. Repeated conformance runs therefore restart the
peripheral rather than assuming that reset happens automatically.

An empty peripheral registry rejects the compiled package before `on_load`, so
it cannot accidentally use test-modeled GATT data. `make check-conformance`
and `make check-gpio` remain separate generic-path checks, not physical radio
evidence.

`firmware/nrf5340_ble_peer/build_bsim.sh` compiles the independent real Zephyr
peripheral and its network-core controller. Running its advertising, ATT,
CCCD, and notification traffic requires the two-core images, two distinct
Bsim devices, and `bs_2G4_phy_v1` in an x86 Bsim/Docker environment. The build
scripts deliberately do not start those processes. Supplying package bytes or a
successful compile does not establish a connection or a radio-interoperability
result. The current ESP32 display/CPU emulator runs likewise do not validate
the NimBLE radio controller.

Endpoint absence, unsupported versions, and missing permissions are rejected.
Package capability declarations and source syntax remain portable; physical
connection outcomes, maximum simultaneous links, packet timing, security state,
and radio interoperability are not made identical across vendors. Bluetooth
Classic and full Web Bluetooth compatibility are outside this interface.
Physical boards need separate RF, controller-concurrency, and resource tests.

### Event links

`event_link` names an owner-allowed hardware route, not an arbitrary vendor
address or a dynamically chosen channel. Metadata exposes `enabled`,
`hardware_autonomous`, and a maintained, exact trigger count within the
specified stop/start lifetime rules. Such a claim requires actual hardware
routing/capture, detected after enablement. GPIO interrupt coalescing, VM timer
polls, or software multiplexed buttons do not qualify as autonomous links.
Group routing (`event_link_groups`) and vendor-specific routing are separate
capabilities; ESP32-S3 must not claim the newer ETM, and Nordic routes use
PPI/DPPI only where implemented and allocated by the owner.

For nRF5340, `platform/zephyr_event_link.*` allocates a GPIOTE event channel
and a GPPI connection. The input event drives both the timer `COUNT` task and
its CC0 capture task. `trigger_count` accumulates captured 32-bit deltas in
`uint64_t`; it is not an interrupt or poll-loop count. More than 2^32 new
events between UI reads cannot be recognized, and no pulse-width/timestamp
fidelity is promised. Counts survive stop/start but reset for a fresh lease.
Open allocates a GPIOTE event channel; close releases it. Stop pauses and start
resumes the timer without clearing its accumulated hardware counter.
The driver resolves `gpio_dt_spec.pin` together with its GPIO port before calling nrfx:
GPIO0 uses that pin index directly; GPIO1 uses Nordic's port-encoded pin mapping.
Unsupported source ports or port-relative indices above 31 are rejected.

On nRF5340 Bsim, only application TIMER0 is accepted. Its custom
`NRF5340_peri_types.h` TIMER structure has eight capture slots and size 1440,
while application MDK's has six and size 1432. Subsequent array entries have
different addresses; using TIMER1 or TIMER2 there would report incorrect counts.
This is a simulator-specific restriction, not a reason to claim TIMER1/TIMER2
work incorrectly on physical boards.
The generic owner interface is `register_zephyr_event_link()` in that header.

The nRF application has an explicit example: `CONFIG_MB_EVENT_LINK_ENABLED=y`
registers `button_event` from the `sw1` alias and uses the owner's TIMER2.
On nRF7002 DK, that alias is GPIO1.9, active-low with pull-up. This option is
off by default. The application manifest must separately grant Event Link 1.0
and `button_event`. The source must not be reassigned while its route is open.
The separate Bsim fixture uses sw1 (GPIO1.9) with application TIMER0 and its own
CSV-driven input. It closes that route before the ordinary GPIO fixture exercises
sw0 (P0.23).

### USB

Device and host roles are separate. Device configuration accepts bounded device
and interface-descriptor records, including interface class/protocol and
endpoints. Host selection uses bounded discovered device addresses; transfers
have typed setup, data, receive-length and timeout bounds. Responses expose
configuration, counts, actual bytes, descriptor bytes, vendor/product IDs and
errors. No raw console/JTAG promotion to application USB, no fabrication of
unplugged devices, and no host role without host hardware and an enabled host
stack.

### Metadata and field lookup

Use the canonical `DeviceMetadataField` IDs in the header for exact numeric
field lookup. BLE peer/descriptor discovery is indexed; scalar configuration
fields are not. Never reinterpret an absent optional Binary/string result as an
empty successful hardware transfer. A Binary field's identity and size are
preserved rather than decoded as UTF-8.

## Capability IDs

| Capability | ID | Scope |
|---|---:|---|
| GPIO | 1 | Input/output configuration, levels, pull, edge subscriptions |
| ADC | 2 | Bounded sample conversion and calibration metadata |
| PWM | 3 | Period/active-time control using hardware timers |
| Pulse capture/output | 4, 5 | Bounded timed/waveform records |
| SPI controller/peripheral | 6, 7 | Named bus roles and bounded binary transactions |
| I²C controller/peripheral | 8, 9 | 7/10-bit addressed transactions and recovery |
| UART/RS-485 | 10, 11 | Configured framed bytes and line errors |
| I²S transmit/receive/PDM receive | 12, 13, 14 | Bounded PCM data and negotiated format |
| BLE central/peripheral | 15, 16 | Discovery/GATT/connection subscription/advertisement |
| Event link/groups | 17, 21 | Allowed hardware-autonomous routes |
| USB device/host | 18, 19 | Descriptor and transfer roles |
| SPI multilane | 20 | Specifically negotiated lane support |
| Espressif GPIO-matrix extension | 22 | Nonportable owner-approved routing only |

These are versioned web-inspired contracts, not implementations of WebGPIO,
WebBluetooth, WebAudio, WebUSB, a W3C DOM, or JavaScript running on the MCU.

## Current port boundaries

| Port | Implemented driver/platform paths | Verified so far | Not verified here |
|---|---|---|---|
| Host + VM | Shared core types; test-only modeled peripherals; host HTTP fixture for native package confirmation | `make check-gpio`: same embedded and confirmed-download artifact finishes GPIO lifecycle twice | OS GPIO, radio, physical timing, signed publisher identity |
| ESP32-S3 | ESP-IDF platform with NVS/asynchronous HTTP; owner-bound GPIO and SPI/I²C/I²S/DMA-safe backend files; `esp_idf_ble_central.*` with the owner-selected NimBLE controller; optional package embedding/download | `make qemu`: dashboard self-test, exact i64/binary NVS reconstruction, actual virtual LCD, emulated Ethernet. `make qemu-gpio`: GPIO subscription and close callbacks, without edge/IRQ claims. The owner and GATT profiles compile native drivers; the GATT artifact has an exact embedded hash | Physical peripheral transactions/IRQs, real panel/touch pins, Wi-Fi, NimBLE controller operation and attached-bus bench checks |
| nRF5340 application core | `platform/zephyr_peripherals.*` GPIO; owner-selected `zephyr_ble_central.*`; full Browser target under `firmware/nrf5340_app`, with Zephyr NVS and asynchronous HTTP package code | `firmware/nrf5340_gpio/bsim.sh`: alternating GPIO edges, rejected input writes, no events after unsubscribe, normal shutdown; GPIO1.9 event-link COUNT/CC0, TIMER1 rejection, stop/restart count preservation and close | Physical board, Wi-Fi/HTTPS, native GATT/radio execution, audio/USB and physical event-link behavior. Building GATT profiles or their controller image alone does not establish these |

The physical `build.sh --gatt` profile expects a raw application image starting
at flash address zero. It reserves 960 KiB for application code and data while
preserving 32 KiB at address 0xf8000 for Wi-Fi settings. The application-core
linker retains 448 KiB of RAM; its remaining 64 KiB is shared with Bluetooth IPC.
This is an explicit non-MCUboot deployment: flashing it replaces the previous
bootloader layout and does not provide MCUboot update slots. A signed FOTA
deployment needs its own measured and tested layout; this profile does not claim it.

The first 128 KiB of the board's external flash holds the nRF70 firmware
patches, which the Wi-Fi driver transfers into Wi-Fi RAM. The application
limits its shared heap to 155,648 bytes; Wi-Fi and TLS retain their dedicated
pools. A complete physical programming operation needs all three artifacts:
the application, `hci_ipc` controller, and `nrf70.hex` patch image. External
storage does not contain the Wi-Fi settings, which stay in the internal area above.

Bsim is an x86 Zephyr execution using the Nordic hardware models, not an ARM
firmware run. Its input schedule is `firmware/nrf5340_gpio/bsim_input.csv`.
The successful sequence reaches `GPIO Bsim alternating-edge notifications
verified`, `GPIO Bsim subscription revocation verified`, and
`GPIO SDK sequence passed for nRF5340 DK sw0 (P0.23)`. Ordinary SDK calls, not
different bytecode or fabricated callback data, drive that test.
The same run also reaches `event-link Bsim COUNT/capture=4, stop/start count
preserved, close completed`. Its source is sw1 (GPIO1.9), exercising the resolved
port encoding. TIMER1 is rejected, and counts are compared before and after
stop/restart.


The completed `firmware/nrf5340_app/bsim.sh` run additionally reports the exact
embedded artifact SHA-256, subscription and four edge notifications, changed
RGB565 frame digests, the unsubscribed/closed application completion, and
`MICROBROWSER_BSIM_PASS`. Its bounded driver-code completion step invokes the
application's existing `finish()` callback, waits for its completion log,
shuts down the GPIO and network backends, and uses the native simulator exit API.
That does not change the application bytes or convert Bsim into a radio.

nRF adaptive application downloads target the nRF7002 Wi-Fi companion family.
Bsim has no nRF7002 radio, TCP/DNS/TLS transport model, so no HTTP success may
be claimed from it. Its headless frame digest proves only that the shared
native renderer produced a frame; it is not evidence of a physical LCD.

`firmware/main/main.cpp` still uses `esp_lcd_rgb_qemu` for its displayed ESP32
reference target. Real ESP32 board deployment needs your actual `esp_lcd`
panel and touch/input adapter wired to `Browser` while keeping the shared VM,
package, callbacks, device sessions, storage and networking unchanged.
Use a board-specific pin/reservation/safe-state profile; do not turn on example
pin numbers merely to request a successful build.

## Struct validation and portability limits

- The native format is MBPK 3.0; older v2 packages remain document-compatible
  but cannot represent hardware permissions or the new values and are rejected
  by hardware-application entry points.
- `Value::Type` additions and compatibility with existing packages must not be
  inferred from parser tolerance. Reject unknown major versions and capabilities.
- The compiler validates static arguments and call signatures; a script can still
  pass a stale/mismatched handle, wrong role, busy queue, offline device, or
  incompatible negotiated clock. Those remain runtime errors/results.
- Native operation support is narrower than the shared protocol matrix.
  Unsupported capabilities return explicit failures/absence; there is no catch-all
  fallback that returns valid data.
- No physical hardware has been attached for this hardware work. Compilation,
  models, QEMU, and Bsim are distinct levels of evidence. Keep loopback,
  oscilloscope/logic-analyzer, radio-interoperability and electrical validation
  separate when a board becomes available.
