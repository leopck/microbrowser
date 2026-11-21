# Portable application and hardware platform plan

Status: selected design and implementation sequence. It does not claim that the following capabilities are already implemented. Supported claims must be backed by the implementation and the conformance matrix.

## Objective and portability guarantee

A publisher builds a website **once** into a portable Microbrowser application package. A device downloads that exact package over HTTPS and executes it without a board-specific build of the website.

The guarantee is:

> An application runs without recompilation on every port that implements its required runtime version, capability versions and resource profile.

Porting Microbrowser to another MCU means implementing the runtime and selected capability contracts. It does not mean rebuilding each website for that MCU.

### Reference ports

| Port | Environment | Application-download path | Event routing |
|---|---|---|---|
| ESP32-S3 | Existing ESP-IDF 6.1 / FreeRTOS port | Existing HTTPS/Ethernet path | Only exposed hardware routes supported by the peripheral/GPIO-matrix configuration; the S3 does not have the newer ETM |
| nRF5340 application core | Nordic Connect SDK / Zephyr, non-secure `cpuapp` reference | nRF7002-class Wi-Fi companion | Nordic DPPI, with explicit channel ownership |

The nRF5340's separate network core remains under the Nordic controller firmware. The application will not load website bytecode into it. A non-TF-M build is the initial reference; secure-partition support requires its own memory and IPC profile.

This plan does not claim every Nordic or Espressif chip is a single hardware target. Later ports can add nRF52/PPI, nRF54/DPPI, TrustZone and other ESP32 variants behind the same application APIs, subject to their own conformance results.

## What “every I/O” means

The v1 hardware API is an explicit matrix for these groups:

- GPIO input/output, pull configuration and interrupt notifications
- ADC conversion, PWM output, hardware timers and pulse capture/output
- SPI controller/peripheral roles; single-wire baseline and separately negotiated multilane extensions
- I²C/TWI controller/peripheral roles, transactions, arbitration failures and bounded recovery
- UART/UARTE configuration, buffered transfers and line errors; separately negotiated RS-485 support
- I²S PCM transmit/receive and separately negotiated PDM capture
- BLE GATT client/server operations, discovery, notification, connection supervision and pairing states
- Event links, including genuine PPI/DPPI-style hardware routing where available
- USB device operation and USB-host operation only on hardware/stack combinations that actually implement them
- Required bus pinmux, DMA ownership and resource arbitration needed to make those operations correct

A role exposed by a capability version must implement every operation its profile declares, with tests. Unsupported physical roles are absent, never successful no-ops. Raw register access, arbitrary callbacks from interrupt context and unbounded vendor operations are not part of the portable API.

Hardware-specific extensions require their own namespace, protocol version and documentation. An application using an Espressif GPIO-matrix extension or Nordic DPPI-specific channel-group extension gives up the guarantee that every other vendor can provide that exact operation. Such extensions never execute outside an approved application and board-owner policy.

These are **versioned Microbrowser API contracts**, not assertions of complete W3C WebGPIO, WebSPI, WebBluetooth, WebAudio or WebUSB compatibility. Their web-inspired request/permission/completion model is documented without pretending that the current typed VM is a browser JavaScript/DOM implementation.

## 1. Portable package and publication ABI

Preserve the existing MBPK version-2 reader and its little-endian primitive types. Existing dashboards remain valid without recompilation.

Introduce a new major format for capability applications, with:

- A VM bytecode version and fixed, architecture-independent instruction encoding
- Immutable package identifiers and resource requirements
- A required/optional capability manifest containing stable numeric identifiers and versions
- Logical peripheral selectors, never native pointers or physical register addresses
- A publisher signature covering the application identity, manifest and package contents
- Resource-budget declarations checked before allocating large buffers
- Explicit payload formats for byte buffers, 64-bit integers and opaque handles

Number, boolean and string tags from version 2 keep their encoding. New value kinds cannot appear in version-2 packages. Major-version incompatibility fails before execution; a port never guesses semantics.

The publisher or its CI system performs compilation, asset preprocessing and signing. The MCU performs only downloading, validation, profile selection and execution. No native Xtensa/ARM executable code, unpacked C++ structure, pointer or absolute hardware address is accepted in an application package.

### Publication sequence

1. Resolve the website URL to a small versioned manifest.
2. Check VM version, required capabilities, resource profile and package size.
3. Present the publisher identity and requested permissions.
4. Download the selected artifact and referenced resources within negotiated bounds.
5. Verify the digest/signature and the exact package byte identity.
6. Refuse execution if required capabilities are missing or a required permission is denied.
7. Start a fresh application generation with its own operation queues and resource leases.
8. Close all leases before loading another application or shutting down the runtime.

Transport HTTPS authenticates the connection, but it is not the application publisher's identity. The signature and owner trust policy provide that distinction. Ordinary Reader documents remain non-executable and do not receive peripheral permissions.

Current native Run consent is retained. A later trusted-application cache can store grants by verified publisher/application identity and permission version, not only a URL or its non-cryptographic hash. New permissions and unverified publisher changes require review again.

## 2. Runtime values and operation protocol

The present `number`, `boolean` and `string` model is insufficient for safe peripheral access. The new ABI adds:

- **64-bit integer values** for counters, timestamps and addresses whose range exceeds binary32 precision
- **Immutable byte buffers** for binary UART/SPI/I²C/PCM data, distinct from UTF-8 strings
- **Opaque handles** for device leases, transfers and event links
- **Absent optional values** for explicitly declared handle/resource state

A handle carries an application generation, slot and object generation. It is not forgeable by assigning a number or loading persistent state. Incrementing object generations prevents a closed or disconnected resource from being reused by a stale handle. Handles are not persisted across application launches.

Buffers have explicit size limits. Drivers retain immutable buffers only for the lifetime of their transfer. A DMA driver either takes an adequately allocated buffer or copies into its own bounded DMA-capable buffer; it never uses unmanaged PSRAM or freed VM storage as a DMA source.

### Completion model

All potentially blocking work follows the existing HTTP pattern:

1. Validate the command, permission, resource role and payload synchronously.
2. Queue it in bounded storage and return an operation handle.
3. Perform physical/vendor work off the UI task, or in an interrupt-safe driver callback.
4. Deliver a bounded completion through the application event queue.
5. Invoke registered application completion handlers on the UI task.

Hardware interrupts do not execute bytecode. Event queues have explicit overflow results, backpressure and coalescing rules. The application cannot force dynamic allocation by flooding GPatt notifications, GPIO edges or USB descriptors.

Completion, cancellation, device removal, timeout and application shutdown have documented ordering. Closing an application releases timers, DMA buffers, GATT connections/subscriptions, audio streams, USB interfaces, event channels and configured pins according to the board's safe-state policy.

The current 20,000-instruction event budget remains a CPU-work bound, not a real-time guarantee. Peripheral timing, radio scheduling and PCM frames are owned by hardware controllers, DMA and vendor drivers, not VM busy loops.

## 3. Capability and permission enforcement

The host provides immutable descriptions of supported capability versions and actual resource limits. Applications can query these descriptions to select documented fallbacks before requesting a handle.

Each operation is checked at both compilation and runtime for:

- Capability/version availability
- Permission against the signed manifest and board-owner policy
- Logical-device ownership
- Correct handle type and generation
- Typed arguments, fixed-width integer ranges and bounded buffer lengths
- Valid role, mode and data direction
- Available queue, memory and peripheral channel capacity

No website text can modify the owner-provided pin map, reserved Wi-Fi/flash pins, radio resources or trust anchors. Reserved peripherals and bus-sharing rules are part of the board profile, not hidden global flags.

### Required profiles and numerical behavior

Profiles describe display dimensions/format, tile-buffer bytes, application bytes, image bytes, network transport, concurrent transfers and feature availability. The runtime exposes actual clocks, frequency resolution and timeout domains rather than pretending that requested and actual frequencies must always be equal.

Numeric parameters with physical units carry units in their names (`period_ns`, `frequency_hz`, `microvolts`, `baud`). Enumerations represent polarity, direction and mode instead of overloaded booleans. Arithmetic and counters do not silently pass through binary32 when exact 64-bit information is needed.

The graphic baseline retains RGB565 little-endian data. Small-memory ports use bounded rectangular render/tile buffers rather than assuming the existing full-screen allocation. Screen size and layout are runtime properties. Movement to a monochrome or otherwise incompatible display requires an explicit rendering profile, not a silent promise of identical pixels.

## 4. Shared peripheral contracts

### GPIO, ADC, PWM and timers

Applications address logical endpoints such as `status_led`, `user_button` or `board_analog_input`. The board resolves pins, polarity, pull configuration and safe states. The owner can prohibit strapping, flash, console, radio and power-control pins.

PWM uses period/duration units and exposes actual achievable values. ADC exposes resolution, reference and conversion information; a reading is not falsely advertised as calibrated when the port lacks calibration. Pulse capture and waveform output use hardware timers/counters, PCNT/RMT or PPI/DPPI-linked equivalents where present, not VM-generated waveforms.

### SPI, I²C and UART

SPI modes, data direction, chip-select ownership, bit order and transfer completion are defined. Multilane modes are separately negotiated and restricted to compatible hardware and pin assignments. Wi-Fi companion buses reserved by the port are not also available to arbitrary applications.

I²C writes, reads and write/read transactions specify stop/repeated-start behavior, address width, NACK, arbitration loss, timeout and recovery. Clock stretching cannot create an unbounded UI or driver wait.

UART configuration includes pins, frame format, actual baud, optional flow control and error reporting. Reads and writes use bounded byte buffers and report overflow, framing and parity errors. RS-485 direction control is exposed only through an implemented hardware/driver configuration.

### I²S, PCM and audio

The baseline is bounded PCM transaction/stream operation, not arbitrary WebAudio node graphs or AudioWorklets. Formats specify sample frame representation, channel count, rate and buffer duration. A port returns the actual supported format or rejects the request.

DMA-owned ring blocks and their completion lifetimes are explicit. Audio callbacks cannot enter the VM directly. On shutdown, the driver stops/cancels streams without dereferencing an application buffer after release. Separate PDM capture is negotiated where the selected Nordic or Espressif peripheral supports it.

### BLE GATT

Client and server roles are separate capabilities. Operations cover filtered discovery, connections, service/characteristic discovery, reads, writes, subscriptions, advertising, notifications, disconnection and connection-state events. Binary attributes use bounded immutable buffers.

The Nordic implementation uses its Zephyr Bluetooth interfaces and nRF5340 network-core controller. Espressif uses its supported BLE host/controller stack. Pairing state and owner permission are explicit. Neither port claims Bluetooth Classic or public-Internet connectivity merely because it has BLE.

### Event links and DPPI/PPI

Applications request a named logical route from an allowlisted event to an allowlisted action. They cannot supply raw endpoint addresses or arbitrary channel numbers.

The DPPI backend owns allocation, endpoint installation, enable/disable and teardown. Reserved channels used by radio, Wi-Fi, USB or other system services can return `busy` or `disallowed`, not be stolen. Group operations require their own versioned contract.

A genuine hardware-autonomous route is advertised only when the peripherals support it. Mere GPIO multiplexing is not described as DPPI. ESP32-S3 implementations register valid peripheral/DMA/GPIO-matrix routes and clearly reject tasks that would require an unavailable event-task matrix or an unsafe software emulation.

### USB

USB-device and USB-host roles are distinct. Descriptors, interfaces, endpoints, transfer direction, ownership and hot-unplug behavior are validated. Protected buses and console/JTAG ownership cannot be overridden by an application.

The nRF5340 reference does not advertise USB host. ESP32-S3 host support requires an actual host stack and correct external VBUS/connector circuitry. GPIO debugging output, USB-device operation and USB-host hosting are never conflated.

## 5. Porting layers

The existing `Platform` interface remains the starting point for monotonic time, asynchronous HTTP and storage. Work proceeds by separating:

1. Application identity, capabilities and permission decisions
2. Bounded operation submission and result delivery
3. OS scheduling/synchronization and driver callback adaptation
4. Logical board endpoints and owner-reserved resources
5. Display/input presentation
6. Vendor driver implementations

The shared VM and DOM-like UI tree do not include FreeRTOS, Zephyr, ESP-IDF or nrfx headers. Vendor drivers are registered only when their implementation and resource restrictions are available. Functional decomposition and typed operation variants avoid one giant stringly typed `do_everything` entry point.

## 6. Conformance and observable evidence

A hardware-independent conformance application and test fixtures cover:

- Package bytes, version rejection and unchanged application artifact identity
- Every advertised operation, argument validation and binary payload, including non-UTF-8 bytes
- Handle forgery, stale handle reuse, disconnects, cancellation and application replacement
- Queue overflow, payload limits, memory limits and stale completion delivery
- Permission denial and attempts to access reserved peripherals
- Resource cleanup and owner-designated safe pin states
- Event-channel allocation/deallocation and torn-down routes
- Restartability after failed device/driver initialization

Tests run against the desktop modeled board, ESP32-S3 in QEMU, and nRF5340 BabbleSim where its peripheral models support the operation. The BabbleSim build is not treated as an ARM firmware build. Generic `native_sim` is not treated as evidence of real DPPI behavior.

The publication tests compare a downloaded package's identity against the original publisher artifact. There is no hidden source compilation in the device loader. Unknown capability versions and incompatible memory profiles fail before execution.

Physical tests need actual boards, the Wi-Fi companion, a logic analyzer or loopback fixtures, appropriate USB circuitry and measured audio/peripheral checks. Unavailable equipment is reported as an unverified physical test, never as a successful hardware test. ESP32's emulated Ethernet and nRF BabbleSim do not validate radio interoperability or electrical correctness.

### Implemented and checked so far

The implementation is narrower than the full capability plan; native driver
availability, model validation, emulator execution and physical tests are kept
separate. See [hardware-api.md](hardware-api.md) for exact contracts and current
port boundaries.

- Host: `make check-gpio` passes. The same 1,393-byte MBPK 3.0 artifact completes
  its GPIO lifecycle through the Browser VM, both embedded and through the
  explicitly confirmed native-download path. Its peripheral and HTTP responses
  are host models, not physical devices or a radio simulation.
- Nordic GPIO: `make nrf-gpio-sim` builds the real Zephyr adapter and executes it
  with the Nordic nRF5340 Bsim model. The observed logs confirm the alternating
  edge sequence, a quiescent interval after unsubscribe, successful shutdown,
  and `GPIO SDK sequence passed for nRF5340 DK sw0 (P0.23)`. The simulator now
  exits through the real `nsi_exit` path after that validation.
- Event links resolve GPIO port/index pairs to Nordic's port-encoded pin before
  nrfx calls. A Bsim register-layout mismatch exists between the custom TIMER
  structure (eight capture slots, 1440 bytes) and application MDK (six slots,
  1432 bytes). The affected Bsim board accepts TIMER0 only and rejects TIMER1/2
  instead of reporting an incorrect count. Stop uses `nrfx_timer_pause()` and
  start uses `nrfx_timer_resume()` so reading and restarting preserve the count;
  full resource release still occurs on close.
- GPIO1.9 event-link COUNT/CC0 capture, TIMER1 rejection, stop/restart count
  preservation, and close are verified by the same Bsim script.
- The nRF7002 GATT profile uses a raw direct-boot layout, not an MCUboot/FOTA
  layout. Flash, settings and application-RAM boundaries are specified in
  [hardware-api.md](hardware-api.md#current-port-boundaries). Image headroom
  still requires bring-up measurements; compilation is not a runtime budget proof.
- Nordic C++ confinement remains explicit: worker/ISR paths exchange bounded POD
  records; requests, variants, C++ completion pointers and application callbacks
  are handled on the constructing application/UI thread.
- Physical BLE-central backends are implemented for ESP32-S3 NimBLE and nRF5340
  Zephyr, using real SDK controller operations and explicit authorization.
  Native discovery uses SDK ATT results and connection-generation-scoped
  attributes rather than fabricated handles or modeled completions. See
  [hardware-api.md](hardware-api.md#ble-gatt) for endpoint limits and the exact
  meaning of security-state metadata.
- `examples/gatt_conformance.mb` compiles to a 5,242-byte artifact with 30
  functions. The ESP32-S3 GATT firmware builds with that exact artifact and
  reports SHA-256 `f192e5df9416c65e94ddc75e42093d000dd1915b549d9ef84287e17c741701ad`.
  It verifies actual initial/read-back bytes, a rejected read-only write,
  peer-triggered notification payload, disconnection, and cleanup. The fixture
  peer's source uses real Zephyr GATT attributes and CCCD handling. This is not
  evidence of an attached radio or interoperability with a physical board.

- ESP32: `platform/esp_idf_peripherals.cpp`, `esp_idf_spi.*`, `esp_idf_i2c.*` and
  `esp_idf_i2s.*` implement owner-selected real peripherals and DMA/event paths.
  Per-family registries compose into one board-owned `PeripheralRegistry`.
  Compilation and peripheral/driver validation must not be inferred from those
  source files alone. The pre-existing graphical QEMU framebuffer and emulated
  Ethernet results are separate from physical-bus validation.

Firmware commands and selected outputs:

```sh
make qemu
make qemu-gpio
firmware/run.sh --build-only --owner-profile firmware/sdkconfig.owner_example
make nrf-app-sim
make nrf-app
```

`run.sh --smoke` builds and tests the selected package using the exported
ESP-IDF environment. `qemu-gpio` uses `build/firmware-gpio-conformance` and
checks the GPIO application's subscription and unsubscribe/close callbacks in
QEMU. It does not inject physical GPIO edges and is not an emulated IRQ
conformance result. `owner_example` compiles the statically selected GPIO,
SPI, I2C and I2S endpoints; verify every pin against the attached schematic.
The normal application embeds `build/dashboard.mbp` and enables no hardware
endpoints by default.

The same GATT package can be built and embedded in the two native profiles:

```sh
python3 -m tools.mbc build examples/gatt_conformance.mb \
  -o build/gatt_conformance.mbp
firmware/run.sh --owner-profile firmware/sdkconfig.gatt_central \
  --build-only --package build/gatt_conformance.mbp
MICROBROWSER_PACKAGE="$PWD/build/gatt_conformance.mbp" \
  firmware/nrf5340_app/build.sh --gatt nrf7002dk/nrf5340/cpuapp
```

Nordic `build.sh --gatt` uses `firmware/nrf5340_app/gatt.conf` and builds the
real `hci_ipc` network-core controller alongside the application. The Bsim
build script also prepares the two core images, but does not start the two
simulated devices or the PHY. An actual radio test requires Linux/x86 Bsim with
Docker plus both independently running devices, each with its app/network
images. `firmware/nrf5340_ble_peer/README.md` supplies the real peer protocol.

`build.sh` forwards an explicitly supplied `MICROBROWSER_PACKAGE` into the
application's cached configuration, so reusing a build directory cannot keep an
earlier GPIO package. An additional `-DMICROBROWSER_PACKAGE=...` supplied by a
caller remains the final CMake override. Supply exact owner-selected bytes;
neither package selection nor compiler success validates a physical peripheral.



`nrf-app-sim` executes the same published GPIO artifact with Nordic's Bsim
driver/input model. `nrf-app` compiles the nRF7002DK nRF5340 application-core
image for its nRF7002-class Wi-Fi companion. Neither emulates the Wi-Fi link,
physical LCD or input hardware. Hardware purchases and their model selection
remain porting choices; the nRF/portable package is not generic nRF52840 firmware.
- Unverified: physical button/bus/electrical behavior, nRF7002 Wi-Fi/HTTPS,
  physical LCD/touch panels, BLE/audio/USB and signed application-publisher
  identity. Bsim validates its modeled nRF Zephyr path, not radio or an ARM
  hardware run.

Compilation alone never marks a peripheral's runtime behavior validated.

## 7. Implementation sequence

1. Add the runtime/capability/value contracts and preserve version-2 compatibility.
2. Add the publisher manifest, signing/verification and profile negotiation.
3. Add bounded handles/buffers, async completions and cleanup to the core and desktop modeled board.
4. Define the compiler's versioned operations and conformance fixture applications.
5. Add portable display/input boundaries and resource-profile enforcement.
6. Implement ESP32-S3 GPIO, timer/pulse, SPI, I²C, UART, ADC/PWM, PCM, BLE, event-route and applicable USB drivers.
7. Install/pin the Nordic build environment and produce an actual nRF5340 application-core firmware build.
8. Add the nRF5340 Zephyr platform, nRF7002 HTTPS transport and DPPI/peripheral/BLE implementations.
9. Run host, gateway, QEMU, nRF build and applicable BabbleSim tests.
10. Record physical-validation requirements and results honestly for operations not exercised on boards.

No operation is marked complete merely because a declaration or stub exists.

## 8. Documentation deliverables

The usage and contract documentation will include:

- Authoring, one-time compilation, HTTPS publishing, signing, trust stores and application updates
- VM/package ABI versioning and backward compatibility
- Capability discovery, required/optional declarations and resource profiles
- GPIO/interrupts, ADC/PWM, pulse I/O and timers
- SPI, I²C and UART transactions, roles, addressing and example payloads
- PCM/I²S/PDM format and streaming lifetimes
- BLE GATT examples, permissions, notifications and pairing/cleanup behavior
- Event links, DPPI/PPI resource allocation and Espressif routing differences
- USB descriptors, device/host roles, transfer ownership and physical requirements
- Board configuration, protected peripherals, adding new drivers and adding a new port
- The full conformance matrix, software evidence and physical-validation limits

Examples use the same published application bytes on compatible desktop, ESP32-S3 and nRF5340 ports. Any exception is identified as a new publisher build, so an accidental board-specific application fork cannot be mistaken for portability.
