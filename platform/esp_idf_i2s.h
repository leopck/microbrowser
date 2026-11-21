#pragma once

#include "microbrowser/peripherals.h"

#include "driver/gpio.h"
#include "driver/i2s_pdm.h"
#include "driver/i2s_std.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace mb {

inline constexpr CapabilityVersion esp_idf_i2s_transmit_version{
    Capability::i2s_transmit, peripheral_abi_major, 0};
inline constexpr CapabilityVersion esp_idf_i2s_receive_version{
    Capability::i2s_receive, peripheral_abi_major, 0};
inline constexpr CapabilityVersion esp_idf_pdm_receive_version{
    Capability::pdm_receive, peripheral_abi_major, 0};
inline constexpr CapabilityVersion esp_idf_i2s_event_link_version{
    Capability::event_link, peripheral_abi_major, 0};

enum class EspPcmRole : std::uint8_t { transmit, receive, pdm_receive };
enum class EspI2sDmaEvent : std::uint8_t {
    sent,
    received,
    receive_overflow,
    send_overflow,
};

[[nodiscard]] i2s_std_clk_config_t esp_i2s_default_standard_clock() noexcept;
[[nodiscard]] i2s_std_slot_config_t esp_i2s_default_standard_slots() noexcept;
[[nodiscard]] i2s_chan_config_t esp_i2s_default_channel_config() noexcept;

// One configuration exclusively owns one simplex I2S direction, or both
// directions of one standard controller when full_duplex is true. A duplex
// endpoint has one portable role but owns both physical handles: transfer can
// then return real peer-channel data. Simplex endpoints do not advertise
// transfer because the protocol response requires received PCM frames.
//
// The owner fixes the controller, standard communication signature, clocks and
// pins before any application runs. Configure can change only sample rate,
// channel_count (one/two) and signed 16/24/32-bit sample width within the
// allowlist. slot masks and all other timing/polarity fields remain fixed.
// signed_24_le uses the ESP32-S2/S3-and-later packed three-byte IDF buffer
// representation. ESP32 four-byte 24-bit representation is rejected
// rather than silently repacked. float_32_le is unsupported.
struct EspI2sEndpointConfig {
    std::string_view logical_name;
    EspPcmRole role = EspPcmRole::transmit;
    bool full_duplex = false;
    i2s_chan_config_t channel = esp_i2s_default_channel_config();
    i2s_std_clk_config_t standard_clock = esp_i2s_default_standard_clock();
    i2s_std_slot_config_t standard_slots = esp_i2s_default_standard_slots();
    i2s_std_slot_config_t duplex_slots = esp_i2s_default_standard_slots();
    i2s_std_gpio_config_t standard_gpio = {
        .mclk = GPIO_NUM_NC,
        .bclk = GPIO_NUM_NC,
        .ws = GPIO_NUM_NC,
        .dout = GPIO_NUM_NC,
        .din = GPIO_NUM_NC,
        .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
    };
#if SOC_I2S_SUPPORTS_PDM_RX
    i2s_pdm_rx_clk_config_t pdm_clock = I2S_PDM_RX_CLK_DEFAULT_CONFIG(16000);
#endif
    gpio_num_t pdm_clock_pin = GPIO_NUM_NC;
    gpio_num_t pdm_data_pin = GPIO_NUM_NC;
#if SOC_I2S_SUPPORTS_PDM_RX
    i2s_pdm_slot_mask_t pdm_slot_mask = I2S_PDM_SLOT_LEFT;
#endif
    std::span<const PcmSampleFormat> allowed_formats;
    std::uint8_t channel_count_mask = (1U << 1) | (1U << 2);
    std::uint32_t minimum_frame_rate_hz = 8000;
    std::uint32_t maximum_frame_rate_hz = 48'000;
    std::uint32_t maximum_frames_per_block = 240;
    std::uint32_t maximum_block_bytes = 4096;
    std::size_t maximum_pending_operations = 4;
    std::uint32_t io_timeout_ms = 1000;
    // ESP-IDF xTaskCreate measures this value in bytes, not StackType_t words.
    std::uint32_t worker_stack_bytes = 6144;
    unsigned worker_priority = 5;

    // Optional owner-named observation of genuine on_sent/on_recv/DMA-overflow
    // callbacks from this same reservation. This is not an ESP ETM route or an
    // application-selected GPIO-matrix/software event route. Empty disables it.
    std::string_view event_logical_name;
    EspI2sDmaEvent event = EspI2sDmaEvent::sent;
};

// Invalid static policy throws std::invalid_argument. Defaults use exact
// I2S_NUM_0 / master / DMA, MSB stereo, and mclk=384*rate so 16/24/32-bit stereo
// clock ratios remain integral; the board still has to supply real pins and an
// explicit allowed_formats span.
void validate_esp_idf_i2s_config(const EspI2sEndpointConfig& config);

// configure destroys/recreates only disabled channels and reports the actual
// rate derived from i2s_channel_get_info().mclk_hz. Samples are copied byte for
// byte, with no implicit channel/sample conversion. Writes must contain whole
// nonempty frames; reads and transfers request 1..configured maximum frames.
// The worker's preallocated DMA-capable source/result blocks are bounded by
// maximum_block_bytes. Results allocate immutable response buffers only after
// the actual read has completed. A short result not aligned to whole frames is
// an I/O failure, never a truncated successful frame.
//
// Destruction and accepted close retire pending requests, wait for an executing
// blocking IDF read/write to return, disable enabled handles, unregister DMA
// callbacks and delete each remaining handle. io_timeout_ms must be nonzero so
// shutdown cannot wait forever for another read/write request.
class EspIdfI2sBackend final : public PeripheralBackend {
public:
    explicit EspIdfI2sBackend(const EspI2sEndpointConfig& config);
    ~EspIdfI2sBackend() override;

    EspIdfI2sBackend(const EspIdfI2sBackend&) = delete;
    EspIdfI2sBackend& operator=(const EspIdfI2sBackend&) = delete;
    EspIdfI2sBackend(EspIdfI2sBackend&&) = delete;
    EspIdfI2sBackend& operator=(EspIdfI2sBackend&&) = delete;

    [[nodiscard]] std::span<const DeviceOperation> operations() const noexcept override;
    IoStatus start_operation(
        const BackendOperation& operation,
        std::weak_ptr<PeripheralCompletionSink> completions) override;
    IoStatus cancel_operation(const BackendOperation& operation) override;
    void release_application(std::uint64_t owner_generation) override;

private:
    friend class EspIdfI2sDmaEventBackend;
    struct State;
    std::unique_ptr<State> m_state;
};

// Generic EventLink open/read/start/stop/close over a monotonically increasing
// count of the selected real I2S DMA ISR event. start/stop gate counting without
// fabricating an action channel; trigger_count is retained across stop/start
// and hardware_autonomous is true because the source is peripheral DMA.
class EspIdfI2sDmaEventBackend final : public PeripheralBackend {
public:
    explicit EspIdfI2sDmaEventBackend(
        std::shared_ptr<EspIdfI2sBackend> source);
    ~EspIdfI2sDmaEventBackend() override;

    EspIdfI2sDmaEventBackend(const EspIdfI2sDmaEventBackend&) = delete;
    EspIdfI2sDmaEventBackend& operator=(const EspIdfI2sDmaEventBackend&) = delete;

    [[nodiscard]] std::span<const DeviceOperation> operations() const noexcept override;
    IoStatus start_operation(
        const BackendOperation& operation,
        std::weak_ptr<PeripheralCompletionSink> completions) override;
    IoStatus cancel_operation(const BackendOperation& operation) override;
    void release_application(std::uint64_t owner_generation) override;

private:
    std::shared_ptr<EspIdfI2sBackend> m_source;
};

// Owns native PCM endpoints, optional DMA-event endpoints, and references the
// board's shared registry. Register no endpoint for an empty span. Each static
// controller and non-NC GPIO is reserved once, except an IDF duplex loopback.
// Declare the shared registry before this object and this object before Browser.
class EspIdfI2sRegistry final {
public:
    EspIdfI2sRegistry(PeripheralRegistry& registry,
                      std::span<const EspI2sEndpointConfig> endpoints);

    EspIdfI2sRegistry(const EspIdfI2sRegistry&) = delete;
    EspIdfI2sRegistry& operator=(const EspIdfI2sRegistry&) = delete;
    EspIdfI2sRegistry(EspIdfI2sRegistry&&) = delete;
    EspIdfI2sRegistry& operator=(EspIdfI2sRegistry&&) = delete;

    [[nodiscard]] PeripheralRegistry& registry() noexcept { return m_registry; }

private:
    PeripheralRegistry& m_registry;
    std::vector<std::shared_ptr<EspIdfI2sBackend>> m_backends;
};
} // namespace mb
