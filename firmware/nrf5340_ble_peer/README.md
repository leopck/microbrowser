# Real nRF GATT peer fixture

This isolated Zephyr application is a **test peripheral**, not an implementation of
Microbrowser's `ble_peripheral` runtime capability. It has no VM, host callback, or
mocked GATT path. Advertising, ATT reads/writes, CCCD handling, and notifications all
use the NCS 3.4.2 Bluetooth host and the generic Zephyr `hci_ipc` network-core image.

## Live behavior

The device starts connectable, scannable undirected advertising as `MB_BLE_PEER`.
Advertising contains General Discoverable/no-BR-EDR flags and the complete 16-bit
service UUID list `ff00`; the complete name is in the scan response. It accepts one
connection at a time, advertises again after the connection object is recycled, and
does not enable SMP, pairing, bonding, or privacy.

| Service | Characteristic | Properties and permissions | Value |
| --- | --- | --- | --- |
| `ff00` | `ff01` | Read, Write Request, Write Without Response, Notify; unauthenticated read/write | Initially `00 ff 80` |
| `ff00` | `ff02` | Read only; unauthenticated read | Always `7f 00 ff` |

`ff01` holds at most 20 bytes. A successful write of 1–20 bytes at offset zero
replaces the entire value and its length; a following read returns exactly those
bytes. Zero-length writes and writes longer than 20 bytes fail with ATT
`INVALID_ATTRIBUTE_LEN` (0x0d). Nonzero-offset writes fail with ATT `INVALID_OFFSET`
(0x07). Both Write Requests and Write Without Response use the same real SDK write
callback. For example, after writing `c3 28 00 ff`, a read returns exactly those four
bytes. The value remains in static memory across disconnects but resets on reboot.
It is not persisted.

`ff02` has no write callback, write permission, Notify property, or CCCD. Attempts to
write it therefore fail through the actual ATT permission handling; it cannot notify.
Its read value never changes during a run.

### Deterministic notification trigger

Subscribing to `ff01` enables its ordinary managed Zephyr CCCD. The subscription
alone deliberately sends **no** notification, so the central can finish its subscribe
operation before requesting the test event.

After subscribing, write exactly `80 00 ff c3 28` to `ff01`, with either write mode.
That write also becomes the new readable value. It submits one static `k_work` item;
the work calls real `bt_gatt_notify()` for `ff01` with those same five bytes. There is
no timer, generated response, or runtime object in this path. Deferring from the ATT
write callback to the system workqueue keeps notification submission out of the GATT
callback and gives deterministic peer-controlled ordering: subscribe, trigger write,
notification. A disable or disconnect before the work runs suppresses it. Repeated
trigger writes while the single work item is still pending coalesce, and a failed
`bt_gatt_notify()` is logged rather than retried without bound.

This trigger makes an exact interoperability assertion possible without racing an
unsolicited onboarding notification. It does not promise delivery after link loss or
resource failure; such failures must not be reported as passing notifications.

## UUID representation checked against the installed SDK

In NCS 3.4.2, `struct bt_uuid_16.val` stores the UUID in host endianness, while
`BT_UUID_16_ENCODE()` expands to `BT_BYTES_LIST_LE16()` for advertising. The source
therefore uses `BT_UUID_DECLARE_16(0xff00)` for GATT declarations and
`BT_UUID_16_ENCODE(0xff00)` for advertising. On the air the `ff00` UUID octets are
`00 ff` (LSB first), not the 16-byte vendor-UUID spelling. The other UUIDs likewise
use the numeric values `ff01` and `ff02`. These are the same values as the readable
fixture model, but this project independently realizes them through Zephyr's real
attribute tables and packet flow.

## Configuration and images

`prj.conf` enables only the application-side peripheral requirements and limits the
host to one connection. `Kconfig.sysbuild` selects NCS's Bluetooth network-core
default (`NRF_DEFAULT_BLUETOOTH`), which chooses `NETCORE_HCI_IPC`. Consequently the
controller source is the installed
`.tools/nrfconnect/ncs/zephyr/samples/bluetooth/hci_ipc` application; no SDK source is
copied here. Both images are built by sysbuild. The application needs both the app
core and the network core at run time.

### Bsim build (not a radio run)

```sh
firmware/nrf5340_ble_peer/build_bsim.sh
```

The script uses the same x86 Bsim container default as the other Nordic builds and
writes ignored artifacts below `.tools/nrfconnect/build/nrf5340bsim-ble-peer/`:

- combined application/network-core executable:
  `nrf5340_ble_peer/zephyr/zephyr.exe`
- application image: `nrf5340_ble_peer/zephyr/zephyr.{elf,hex}`
- network-core image: `hci_ipc/zephyr/zephyr.{elf,hex}`

The executable's native simulator configuration links the application ELF with the
separate `hci_ipc` ELF for its second emulated CPU. Preserve the listed directory
layout when running it. Start the complete test in an x86 Bsim environment with the
same `/build` mount used by the build script.

**Building is not running the two-peer radio test.** The build script starts neither
`bs_2G4_phy_v1` nor any device executable. A complete radio test additionally needs
one Bsim PHY instance and two distinct simulated nRF5340 devices: this peripheral and
the central, each with its own app/network-core images, simulation ID, and device
number. The parent integration owns that launch and its result checks. Running this
application alone proves no peer interaction.

### Optional physical nRF7002 DK build (not hardware validation)

```sh
firmware/nrf5340_ble_peer/build_nrf7002dk.sh
```

The board target is `nrf7002dk/nrf5340/cpuapp`; `NCS_ROOT`, `NCS_TOOLCHAIN`, and
`NCS_PYTHON` can point at another NCS 3.4.2 installation, and `NCS_BUILD_DIR` can
relocate the output. By default, images are written below
`.tools/nrfconnect/build/nrf7002dk-ble-peer/`:

- application core: `nrf5340_ble_peer/zephyr/zephyr.{elf,hex}`
- network core: `hci_ipc/zephyr/zephyr.{elf,hex}`

Both cores must be flashed for a board run. The script only compiles; it does not
flash, enumerate a board, connect a central, or establish physical RF/interoperability
results. No physical hardware claim is implied by supplying this configuration.

## Limits and evidence

This fixture supplies deterministic bytes for a real SDK GATT-central test. It does
not model scan results, RSSI, addresses, connection state, pairing status, or GATT
success in host-test code, and it supplies no evidence about physical radio timing,
coexistence, nRF7002 Wi-Fi, or interoperability with hardware. Those claims require a
separately observed Bsim radio run or physical-board run, respectively.
