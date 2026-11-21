#pragma once

#include "microbrowser/peripherals.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace mb {

inline constexpr CapabilityVersion esp_idf_ble_central_version{
    Capability::ble_central, peripheral_abi_major, 0};

inline constexpr std::size_t esp_idf_ble_central_max_endpoints = 4;
inline constexpr std::size_t esp_idf_ble_central_max_pending_operations = 8;
inline constexpr std::size_t esp_idf_ble_central_max_subscriptions_per_endpoint = 8;
inline constexpr std::size_t esp_idf_ble_central_max_scan_results = 16;
inline constexpr std::size_t esp_idf_ble_central_max_services = 16;
inline constexpr std::size_t esp_idf_ble_central_max_characteristics = 32;
inline constexpr std::size_t esp_idf_ble_central_max_advertisement_bytes = 255;
inline constexpr std::size_t esp_idf_ble_central_max_attribute_bytes = 512;
inline constexpr std::size_t esp_idf_ble_central_max_logical_name_bytes = 63;

// Every logical name is an owner-declared application route. Each route has at
// most one open DeviceHandle and one peer connection. Durations are fixed by
// the embedding owner rather than application bytecode and must be 1..120000 ms.
struct EspBleCentralEndpointConfig {
    std::string_view logical_name;
    std::uint32_t scan_duration_ms = 10'000;
    std::uint32_t connect_timeout_ms = 30'000;
};

// One backend owns the process-wide NimBLE controller and host. Construct no
// more than one non-empty central backend at a time. Reads and writes bounded
// by BLE_ATT_ATTR_MAX_LEN use real GATT procedures; cancellation of a native
// ATT procedure completes only after NimBLE finishes that procedure.
class EspIdfBleCentralBackend final : public PeripheralBackend {
public:
    explicit EspIdfBleCentralBackend(
        std::span<const EspBleCentralEndpointConfig> endpoints);
    ~EspIdfBleCentralBackend() override;

    EspIdfBleCentralBackend(const EspIdfBleCentralBackend&) = delete;
    EspIdfBleCentralBackend& operator=(const EspIdfBleCentralBackend&) = delete;
    EspIdfBleCentralBackend(EspIdfBleCentralBackend&&) = delete;
    EspIdfBleCentralBackend& operator=(EspIdfBleCentralBackend&&) = delete;

    [[nodiscard]] std::span<const DeviceOperation> operations() const noexcept override;
    IoStatus start_operation(
        const BackendOperation& operation,
        std::weak_ptr<PeripheralCompletionSink> completions) override;
    IoStatus cancel_operation(const BackendOperation& operation) override;
    void release_application(std::uint64_t owner_generation) override;

private:
    struct State;
    std::unique_ptr<State> m_state;
};

// Keep the registry alive longer than this object and this object longer than
// Browser. An empty configuration does not initialize NimBLE or register an
// endpoint. Per-application release only stops that application's routes.
class EspIdfBleCentralRegistry final {
public:
    EspIdfBleCentralRegistry(
        PeripheralRegistry& registry,
        std::span<const EspBleCentralEndpointConfig> endpoints);

    EspIdfBleCentralRegistry(const EspIdfBleCentralRegistry&) = delete;
    EspIdfBleCentralRegistry& operator=(const EspIdfBleCentralRegistry&) = delete;
    EspIdfBleCentralRegistry(EspIdfBleCentralRegistry&&) = delete;
    EspIdfBleCentralRegistry& operator=(EspIdfBleCentralRegistry&&) = delete;

    [[nodiscard]] PeripheralRegistry& registry() noexcept { return m_registry; }

private:
    PeripheralRegistry& m_registry;
    std::shared_ptr<EspIdfBleCentralBackend> m_ble_central;
};

} // namespace mb
