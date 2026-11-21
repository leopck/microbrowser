#pragma once

#include "microbrowser/peripherals.h"

#include "driver/i2c_master.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace mb {

inline constexpr CapabilityVersion esp_idf_i2c_controller_version{
    Capability::i2c_controller, peripheral_abi_major, 0};

// Inclusive raw-address reservation. Seven-bit ranges exclude the reserved
// 0x00..0x07 and 0x78..0x7f addresses. ten_bit selects IDF's native
// I2C_ADDR_BIT_LEN_10 handling; ten-bit requests are never truncated to seven
// bits. Configurations containing ten-bit ranges are rejected on targets
// without SOC_I2C_SUPPORT_10BIT_ADDR.
struct EspI2cAddressRange {
    std::uint16_t first_address = 0x08;
    std::uint16_t last_address = 0x77;
    bool ten_bit = false;
};

// A static board-owner reservation, not an application-selected pin mapping.
// port must name a general-purpose HP controller (0..SOC_HP_I2C_NUM-1); IDF's
// automatic selection (-1) and LP controller IDs are deliberately rejected.
// SDA and SCL must be distinct, bidirectional-capable GPIOs. The board owner is
// responsible for not assigning flash, PSRAM, console, strapping, or otherwise
// occupied pins. Internal pulls are optional and do not replace suitable
// external bus pull-ups. allow_power_down maps to IDF's allow_pd flag.
//
// allowed_addresses is deny-by-default when empty (and an empty span is
// invalid for an endpoint). Applications may select only an address and address
// width in these inclusive ranges. Configure clamps a nonzero requested clock
// to minimum/maximum_frequency_hz. The configure response is the selected
// nominal scl_speed_hz, not a measurement of rise-time-dependent wire speed;
// the IDF 6.1 master API has no actual-frequency query.
//
// maximum_transfer_bytes bounds each read or write phase. Internal TX/RX
// staging storage is preallocated for every pending operation. The IDF master
// uses synchronous CPU-fed hardware FIFOs here, not DMA; staging keeps ISR
// buffers in internal RAM for CONFIG_I2C_ISR_IRAM_SAFE rather than presenting
// caller memory as DMA. IDF xTaskCreate measures worker_stack_bytes in bytes,
// unlike vanilla FreeRTOS stack words. logical_name and allowed_addresses
// backing storage must remain alive and unchanged for the backend lifetime.
struct EspI2cEndpointConfig {
    std::string_view logical_name;
    i2c_port_num_t port = I2C_NUM_0;
    gpio_num_t sda_pin = GPIO_NUM_NC;
    gpio_num_t scl_pin = GPIO_NUM_NC;
    bool enable_internal_pullup = false;
    i2c_clock_source_t clock_source = I2C_CLK_SRC_DEFAULT;
    std::uint8_t glitch_ignore_count = 7;
    int interrupt_priority = 0;
    bool allow_power_down = false;
    std::uint32_t minimum_frequency_hz = 100'000;
    std::uint32_t maximum_frequency_hz = 400'000;
    std::uint32_t scl_wait_us = 2000;
    std::span<const EspI2cAddressRange> allowed_addresses;
    std::uint32_t maximum_transfer_bytes = 4096;
    std::size_t maximum_pending_operations = 4;
    std::uint32_t io_timeout_ms = 1000;
    std::uint32_t worker_stack_bytes = 6144;
    unsigned worker_priority = 5;
};

// Validates immutable owner policy before reserving hardware. Invalid policy
// throws std::invalid_argument. Backend construction additionally throws
// std::runtime_error for an IDF bus, storage, semaphore, or worker failure.
void validate_esp_idf_i2c_config(const EspI2cEndpointConfig& config);

// One backend owns one general-purpose I2C controller and pin pair. The native
// bus is reserved at construction. Open, configure, start/stop, read, write,
// transfer, and close execute serially on one FreeRTOS worker. Configure is
// allowed only while stopped; I/O requires configured and started state. Each
// I/O adds a device using IDF's verified dev_addr_length/device_address fields
// and removes it after the synchronous transaction, so address breadth cannot
// cause an unbounded device cache.
//
// Read and write map to i2c_master_receive/transmit. A transfer requires a
// nonempty write phase. A zero-byte read phase is a write-only transfer. With
// read bytes, repeated_start=true uses i2c_master_transmit_receive, which emits
// no STOP between phases. repeated_start=false instead uses one
// i2c_master_execute_defined_operations call with STOP/START while retaining
// the bus lock; neither form splits a transfer into separate driver calls.
// IDF reports NACK as ESP_ERR_INVALID_RESPONSE and folds arbitration loss into
// its timeout error path. NACK therefore completes with device_lost and
// I2cFault::nack, never as successful empty data; timeout/arbitration failures
// use IoStatus::timeout. The immutable request remains in its work record until
// the blocking IDF call has finished and its device handle has been removed.
//
// Destruction, accepted close, and release_application retire queued work and
// mark active work cancelled. IDF has no synchronous-transfer abort operation,
// so an active FIFO transaction is allowed to finish or reach io_timeout_ms;
// io_timeout_ms must be nonzero and fit int. This gives close/release a finite
// hardware wait, after which no queued cancelled operation enters the driver.
// Accepted close revokes the old lease before this cleanup; rejected close does
// not affect it. Destruction joins the worker before removing any remaining
// device and deleting the master bus.
class EspIdfI2cBackend final : public PeripheralBackend {
public:
    explicit EspIdfI2cBackend(const EspI2cEndpointConfig& config);
    ~EspIdfI2cBackend() override;

    EspIdfI2cBackend(const EspIdfI2cBackend&) = delete;
    EspIdfI2cBackend& operator=(const EspIdfI2cBackend&) = delete;
    EspIdfI2cBackend(EspIdfI2cBackend&&) = delete;
    EspIdfI2cBackend& operator=(EspIdfI2cBackend&&) = delete;

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

// Owns native I2C backends and references the board's shared registry. Declare
// the shared registry before this object and this object before Browser. Empty
// configurations register no endpoint; duplicate logical names, controllers,
// and SDA/SCL reservations are rejected before backends are constructed.
class EspIdfI2cRegistry final {
public:
    EspIdfI2cRegistry(PeripheralRegistry& registry,
                      std::span<const EspI2cEndpointConfig> endpoints);

    EspIdfI2cRegistry(const EspIdfI2cRegistry&) = delete;
    EspIdfI2cRegistry& operator=(const EspIdfI2cRegistry&) = delete;
    EspIdfI2cRegistry(EspIdfI2cRegistry&&) = delete;
    EspIdfI2cRegistry& operator=(EspIdfI2cRegistry&&) = delete;

    [[nodiscard]] PeripheralRegistry& registry() noexcept { return m_registry; }

private:
    PeripheralRegistry& m_registry;
    std::vector<std::shared_ptr<EspIdfI2cBackend>> m_backends;
};

} // namespace mb
