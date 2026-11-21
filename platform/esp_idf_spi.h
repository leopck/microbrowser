#pragma once

#include "microbrowser/peripherals.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace mb {

inline constexpr CapabilityVersion esp_idf_spi_controller_version{
    Capability::spi_controller, peripheral_abi_major, 0};
inline constexpr CapabilityVersion esp_idf_spi_multilane_version{
    Capability::spi_multilane, peripheral_abi_major, 0};

// A bit in the set means the board owner permits that lane configuration.
enum class EspSpiLaneSupport : std::uint8_t {
    single = 1U << 0,
    dual = 1U << 1,
    quad = 1U << 2,
};

[[nodiscard]] constexpr EspSpiLaneSupport operator|(
    EspSpiLaneSupport left, EspSpiLaneSupport right) noexcept {
    return static_cast<EspSpiLaneSupport>(
        static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
}

// A configuration is a static board reservation, not an application-supplied
// pin mapping. host must be SPI2_HOST or a later general-purpose host supported
// by the target; SPI0/SPI1 flash hosts are rejected. bus contains only the
// owner's immutable pins/timing. data4_io_num..data7_io_num must be -1 because
// the portable v1 request has no octal-lane operation.
struct EspSpiEndpointConfig {
    std::string_view logical_name;
    Capability capability = Capability::spi_controller;
    spi_host_device_t host = SPI2_HOST;
    spi_bus_config_t bus = {
        .mosi_io_num = -1,
        .miso_io_num = -1,
        .sclk_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .data_io_default_level = false,
        .max_transfer_sz = 0,
        .dma_burst_size = 0,
        .flags = 0,
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
        .intr_flags = 0,
    };
    gpio_num_t chip_select = GPIO_NUM_NC;
    spi_dma_chan_t dma_channel = SPI_DMA_CH_AUTO;
    spi_clock_source_t clock_source = SPI_CLK_SRC_DEFAULT;
    spi_sampling_point_t sample_point = SPI_SAMPLING_POINT_PHASE_0;
    bool full_duplex = true;
    bool cs_active_high = false;
    int input_delay_ns = 0;
    std::uint16_t duty_cycle_pos = 128;
    std::uint16_t cs_cycles_before_transfer = 0;
    std::uint8_t cs_cycles_after_transfer = 0;
    std::uint32_t maximum_frequency_hz = 1'000'000;
    std::uint8_t allowed_modes = 0x0f;
    EspSpiLaneSupport supported_lanes = EspSpiLaneSupport::single;
    std::uint32_t maximum_transfer_bytes = 4092;
    std::size_t maximum_pending_operations = 4;
    std::uint32_t io_timeout_ms = 1000;
    // ESP-IDF xTaskCreate measures this value in bytes, not StackType_t words.
    std::uint32_t worker_stack_bytes = 6144;
    unsigned worker_priority = 5;
};

// Validates only immutable policy. Constructing the backend additionally
// reserves the SPI bus and allocates bounded internal-DMA staging buffers.
// Both functions throw std::invalid_argument for invalid policy and
// std::runtime_error for an IDF allocation/device-registration failure.
void validate_esp_idf_spi_config(const EspSpiEndpointConfig& config);

// One backend owns one host and one chip-select reservation. Open/configure/
// start/stop/transfer/close run on its FreeRTOS worker. A transfer retains the
// immutable request until completion, but the SPI driver sees only the
// preallocated internal-RAM DMA buffers. Full-duplex transfers zero-extend TX
// to max(tx_bytes, receive_bytes), as required by spi_transaction_t when RX is
// longer than TX. Half-duplex uses independent length/rxlength values. On
// targets such as ESP32-S3 without SOC_SPI_HD_BOTH_INOUT_SUPPORTED, a
// half-duplex request containing both phases is rejected as unsupported;
// separate TX-only and RX-only transactions remain available. No dummy,
// command, address, CS-keep-active, or software-bit-banged operation is added.
//
// The reported frequency uses spi_device_get_actual_freq(), whose IDF API is
// integer kHz. Requests above the owner ceiling are clipped before device
// creation; a negotiated clock above that ceiling is rejected. Configure is
// allowed only while stopped. Destruction and accepted close wait for queued/
// running work to leave the IDF driver before removing the device and bus.
class EspIdfSpiBackend final : public PeripheralBackend {
public:
    explicit EspIdfSpiBackend(const EspSpiEndpointConfig& config);
    ~EspIdfSpiBackend() override;

    EspIdfSpiBackend(const EspIdfSpiBackend&) = delete;
    EspIdfSpiBackend& operator=(const EspIdfSpiBackend&) = delete;
    EspIdfSpiBackend(EspIdfSpiBackend&&) = delete;
    EspIdfSpiBackend& operator=(EspIdfSpiBackend&&) = delete;

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

// Owns native SPI backends and references the board's shared registry. Declare
// the registry before this object and this object before Browser. Empty
// configurations register no endpoint; duplicate names, hosts, and non-NC pins
// are rejected.
class EspIdfSpiRegistry final {
public:
    EspIdfSpiRegistry(PeripheralRegistry& registry,
                      std::span<const EspSpiEndpointConfig> endpoints);

    EspIdfSpiRegistry(const EspIdfSpiRegistry&) = delete;
    EspIdfSpiRegistry& operator=(const EspIdfSpiRegistry&) = delete;
    EspIdfSpiRegistry(EspIdfSpiRegistry&&) = delete;
    EspIdfSpiRegistry& operator=(EspIdfSpiRegistry&&) = delete;

    [[nodiscard]] PeripheralRegistry& registry() noexcept { return m_registry; }

private:
    PeripheralRegistry& m_registry;
    std::vector<std::shared_ptr<EspIdfSpiBackend>> m_backends;
};

} // namespace mb
