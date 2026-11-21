#pragma once

#include "microbrowser/peripherals.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace mb::zephyr {

inline constexpr CapabilityVersion zephyr_ble_central_version{
    Capability::ble_central, peripheral_abi_major, 0};

inline constexpr std::size_t zephyr_ble_central_max_pending_operations = 8;
inline constexpr std::size_t zephyr_ble_central_max_subscriptions = 8;
inline constexpr std::size_t zephyr_ble_central_max_scan_results = 16;
inline constexpr std::size_t zephyr_ble_central_max_notifications = 16;
inline constexpr std::size_t zephyr_ble_central_max_services = 16;
inline constexpr std::size_t zephyr_ble_central_max_characteristics = 32;
inline constexpr std::size_t zephyr_ble_central_max_advertisement_bytes = 1650;
inline constexpr std::size_t zephyr_ble_central_max_attribute_bytes = 512;
inline constexpr std::size_t zephyr_ble_central_max_logical_name_bytes = 63;
inline constexpr std::uint32_t zephyr_ble_central_connect_attempt_timeout_ms = 30'000;
inline constexpr std::uint32_t zephyr_ble_central_shutdown_timeout_ms = 5'000;

// The logical name is an owner-declared route, not an application-supplied
// controller name. Scan timing is bounded independently of HCI scan timeout
// support because active scans cannot use the SDK timeout with every privacy
// configuration. One route owns at most one open application lease and one LE
// central connection.
struct ZephyrBleCentralConfig {
    std::string_view logical_name;
    std::uint32_t scan_timeout_ms = 10'000;
    EndpointAccess access = EndpointAccess::application;
};

// Construction, registration calls, polling, cancellation, shutdown, and
// destruction are confined to the application thread that constructed the
// backend. Nordic callbacks copy only bounded plain records. They never retain
// discovery attributes, advertisement buffers, completion sinks, variants, or
// application byte-buffer pointers. Continuations run on one process-wide
// private k_work_q with an 8 KiB stack. NCS calls that synchronously wait for
// HCI completion are confined to that thread; k_sys_work_q handles only the
// short scan-timeout record handoff and never waits for a controller action.
//
// Each registration consumes fixed storage for eight operations, eight local
// subscription records, sixteen notifications, and the service cache. The
// native work queue persists for process lifetime after the first registration.
// Up to eight backends can be registered. All registrations share one
// 16-result scan store because Zephyr has one scanner. Accepted UUIDs are
// 16-bit or
// 128-bit little-endian values; 32-bit inputs are explicitly rejected. Each
// connected peer permits one native subscription per characteristic.
// Advertisement reports and ATT values larger than the applicable DeviceBudget
// fail the operation with memory_exhausted rather than being truncated.
// Characteristic subscriptions require a per-buffer budget of at least 512
// bytes so every ATT attribute value has an owned destination. A full
// notification queue applies bounded loss without fabricating a partial value.
// Scan results are retained for peer-identity validation until a subsequent
// successful scan replaces them.
// Attempted connection establishment has a 30-second SDK bound, while
// BleConnectRequest controls link supervision.
//
// Primary-service and characteristic discovery are performed with the real
// remote database. Handles, CCCD locations, and connection generations remain
// native implementation details. Read procedures include SDK long reads up to
// 512 bytes. Each write is one real write request/command and is therefore
// limited to ATT_MTU - 3 bytes; larger writes fail instead of emulating an
// unacknowledged long write. The existing paired metadata reports encryption
// at security level L2 or greater; L2 does not necessarily provide MITM
// authentication or a persisted bond, and is not an authentication guarantee.
// This backend does not initiate pairing.
//
// close and release_application suppress their own queued deliveries and
// disconnect only their own connection. They never call bt_disable().
// shutdown() additionally cancels outstanding SDK procedures, waits for their
// callbacks, removes itself from the native connection dispatcher, and is
// idempotent. Drain application
// results before shutdown: shutdown intentionally discards undelivered UI
// results. A shutdown timeout returns timeout and leaves all callback storage
// alive for a retry; destruction after an unsuccessful shutdown terminates
// rather than leaving Nordic with dangling callback pointers. Never call
// shutdown or the destructor from k_sys_work_q, Bluetooth RX, or an ISR.
class ZephyrBleCentralBackend final : public PeripheralBackend {
public:
    ~ZephyrBleCentralBackend() override;

    ZephyrBleCentralBackend(const ZephyrBleCentralBackend&) = delete;
    ZephyrBleCentralBackend& operator=(const ZephyrBleCentralBackend&) = delete;
    ZephyrBleCentralBackend(ZephyrBleCentralBackend&&) = delete;
    ZephyrBleCentralBackend& operator=(ZephyrBleCentralBackend&&) = delete;

    [[nodiscard]] IoStatus shutdown() noexcept;

    [[nodiscard]] std::span<const DeviceOperation> operations() const noexcept override;
    IoStatus start_operation(
        const BackendOperation& operation,
        std::weak_ptr<PeripheralCompletionSink> completions) override;
    IoStatus cancel_operation(const BackendOperation& operation) override;
    void release_application(std::uint64_t owner_generation) override;

    // Delivers queued completions and notifications only on the construction
    // thread. Registration supplies this method to PeripheralRegistry.
    void poll_on_application_thread();

private:
    friend std::shared_ptr<ZephyrBleCentralBackend> register_zephyr_ble_central(
        PeripheralRegistry& registry, const ZephyrBleCentralConfig& config);

    explicit ZephyrBleCentralBackend(const ZephyrBleCentralConfig& config);

    struct State;
    std::unique_ptr<State> m_state;
};

// Initializes Bluetooth synchronously on the application thread only when it
// is not already ready, then registers one real non-modeled BLE-central
// endpoint. Existing Bluetooth users retain controller ownership. Empty names,
// and zero or overflowing finite scan timeouts are rejected before an endpoint
// is registered. The caller must tolerate the native k_work_q's application
// priority and retained stack allocation.
// no partial backend is returned. The returned shared_ptr must outlive the
// registry and all sessions using it, and shutdown must succeed before it dies.
[[nodiscard]] std::shared_ptr<ZephyrBleCentralBackend> register_zephyr_ble_central(
    PeripheralRegistry& registry, const ZephyrBleCentralConfig& config);

} // namespace mb::zephyr
