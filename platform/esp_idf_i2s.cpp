#include "esp_idf_i2s.h"

#include "esp_idf_worker.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace mb {
namespace {

constexpr char kLogTag[] = "mb_i2s";
constexpr std::size_t kMaximumLogicalNameBytes = 63;
constexpr std::uint8_t kAllSignedFormats =
    (1U << static_cast<std::uint8_t>(PcmSampleFormat::signed_16_le)) |
    (1U << static_cast<std::uint8_t>(PcmSampleFormat::signed_24_le)) |
    (1U << static_cast<std::uint8_t>(PcmSampleFormat::signed_32_le));

constexpr std::array<DeviceOperation, 6> kEventOperations{
    DeviceOperation::open, DeviceOperation::start, DeviceOperation::stop,
    DeviceOperation::read, DeviceOperation::close, DeviceOperation::cancel,
};

// cancel_operation is implemented even where the legacy GPIO operation list
// also advertises cancel. Keep it in every actual PCM list without permitting
// DeviceOperation::cancel to be submitted as a request.
constexpr std::array<DeviceOperation, 7> kTxOperationsWithCancel{
    DeviceOperation::open, DeviceOperation::configure, DeviceOperation::start,
    DeviceOperation::stop, DeviceOperation::write, DeviceOperation::close,
    DeviceOperation::cancel,
};
constexpr std::array<DeviceOperation, 8> kTxDuplexOperationsWithCancel{
    DeviceOperation::open, DeviceOperation::configure, DeviceOperation::start,
    DeviceOperation::stop, DeviceOperation::write, DeviceOperation::transfer,
    DeviceOperation::close, DeviceOperation::cancel,
};
constexpr std::array<DeviceOperation, 7> kRxOperationsWithCancel{
    DeviceOperation::open, DeviceOperation::configure, DeviceOperation::start,
    DeviceOperation::stop, DeviceOperation::read, DeviceOperation::close,
    DeviceOperation::cancel,
};
constexpr std::array<DeviceOperation, 8> kRxDuplexOperationsWithCancel{
    DeviceOperation::open, DeviceOperation::configure, DeviceOperation::start,
    DeviceOperation::stop, DeviceOperation::read, DeviceOperation::transfer,
    DeviceOperation::close, DeviceOperation::cancel,
};

[[nodiscard]] IoStatus i2s_status(esp_err_t error) noexcept {
    switch (error) {
    case ESP_OK: return IoStatus::completed;
    case ESP_ERR_INVALID_ARG: return IoStatus::invalid_argument;
    case ESP_ERR_INVALID_STATE: return IoStatus::busy;
    case ESP_ERR_NOT_SUPPORTED: return IoStatus::unsupported;
    case ESP_ERR_TIMEOUT: return IoStatus::timeout;
    case ESP_ERR_NOT_FOUND: return IoStatus::busy;
    case ESP_ERR_NO_MEM: return IoStatus::memory_exhausted;
    default: return IoStatus::io_failure;
    }
}

[[noreturn]] void construction_error(std::string_view action, esp_err_t error) {
    std::string message(action);
    message += ": ";
    message += esp_err_to_name(error);
    throw std::runtime_error(message);
}

[[nodiscard]] Capability capability_of(EspPcmRole role) noexcept {
    switch (role) {
    case EspPcmRole::transmit: return Capability::i2s_transmit;
    case EspPcmRole::receive: return Capability::i2s_receive;
    case EspPcmRole::pdm_receive: return Capability::pdm_receive;
    }
    return Capability::none;
}

[[nodiscard]] CapabilityVersion version_of(EspPcmRole role) noexcept {
    switch (role) {
    case EspPcmRole::transmit: return esp_idf_i2s_transmit_version;
    case EspPcmRole::receive: return esp_idf_i2s_receive_version;
    case EspPcmRole::pdm_receive: return esp_idf_pdm_receive_version;
    }
    return CapabilityVersion{};
}

[[nodiscard]] bool contains(std::span<const DeviceOperation> operations,
                            DeviceOperation id) noexcept {
    return std::find(operations.begin(), operations.end(), id) != operations.end();
}

[[nodiscard]] std::size_t sample_bytes(PcmSampleFormat format) noexcept {
    switch (format) {
    case PcmSampleFormat::signed_16_le: return 2;
    case PcmSampleFormat::signed_24_le: return 3;
    case PcmSampleFormat::signed_32_le:
    case PcmSampleFormat::float_32_le: return 4;
    }
    return 0;
}

[[nodiscard]] i2s_data_bit_width_t data_width(PcmSampleFormat format) noexcept {
    switch (format) {
    case PcmSampleFormat::signed_16_le: return I2S_DATA_BIT_WIDTH_16BIT;
    case PcmSampleFormat::signed_24_le: return I2S_DATA_BIT_WIDTH_24BIT;
    case PcmSampleFormat::signed_32_le: return I2S_DATA_BIT_WIDTH_32BIT;
    case PcmSampleFormat::float_32_le: return I2S_DATA_BIT_WIDTH_32BIT;
    }
    return static_cast<i2s_data_bit_width_t>(0);
}

[[nodiscard]] bool event_has_tx(const EspI2sEndpointConfig& config) noexcept {
    return config.role == EspPcmRole::transmit || config.full_duplex;
}

[[nodiscard]] bool event_has_rx(const EspI2sEndpointConfig& config) noexcept {
    return config.role != EspPcmRole::transmit || config.full_duplex;
}

[[nodiscard]] bool valid_std_slot_width(i2s_slot_bit_width_t width) noexcept {
    return width == I2S_SLOT_BIT_WIDTH_AUTO || width == I2S_SLOT_BIT_WIDTH_8BIT ||
           width == I2S_SLOT_BIT_WIDTH_16BIT || width == I2S_SLOT_BIT_WIDTH_24BIT ||
           width == I2S_SLOT_BIT_WIDTH_32BIT;
}

void validate_standard_pins(const EspI2sEndpointConfig& config) {
#if SOC_I2S_HW_VERSION_2
    const bool external_clock = config.standard_clock.clk_src == I2S_CLK_SRC_EXTERNAL;
#else
    const bool external_clock = false;
#endif
    const bool transmit = config.role == EspPcmRole::transmit || config.full_duplex;
    const bool receive = config.role == EspPcmRole::receive || config.full_duplex;
    const auto& gpio = config.standard_gpio;
    if ((transmit && !GPIO_IS_VALID_OUTPUT_GPIO(gpio.dout)) ||
        (receive && !GPIO_IS_VALID_GPIO(gpio.din)) ||
        (config.channel.role == I2S_ROLE_MASTER &&
         (!GPIO_IS_VALID_OUTPUT_GPIO(gpio.bclk) || !GPIO_IS_VALID_OUTPUT_GPIO(gpio.ws))) ||
        (config.channel.role != I2S_ROLE_MASTER &&
         (!GPIO_IS_VALID_GPIO(gpio.bclk) || !GPIO_IS_VALID_GPIO(gpio.ws))) ||
        (gpio.mclk != GPIO_NUM_NC &&
         (external_clock ? !GPIO_IS_VALID_GPIO(gpio.mclk)
                         : !GPIO_IS_VALID_OUTPUT_GPIO(gpio.mclk)))) {
        throw std::invalid_argument("ESP I2S standard GPIO map is invalid for its direction and clock role");
    }
    const std::array<gpio_num_t, 5> pins{
        gpio.mclk, gpio.bclk, gpio.ws, gpio.dout, gpio.din};
    for (std::size_t index = 0; index < pins.size(); ++index) {
        if (pins[index] == GPIO_NUM_NC) continue;
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (pins[previous] != pins[index] || pins[previous] == GPIO_NUM_NC) continue;
            const bool loopback = config.full_duplex &&
                ((index == 3 && previous == 4) || (index == 4 && previous == 3));
            if (!loopback) throw std::invalid_argument("duplicate ESP I2S standard GPIO assignment");
        }
    }
}

void validate_event_config(const EspI2sEndpointConfig& config) {
    if (config.event_logical_name.empty()) return;
    if (config.event_logical_name == config.logical_name ||
        config.event_logical_name.size() > kMaximumLogicalNameBytes) {
        throw std::invalid_argument("ESP I2S DMA event needs a distinct 1..63-byte name");
    }
    switch (config.event) {
    case EspI2sDmaEvent::sent:
    case EspI2sDmaEvent::send_overflow:
        if (!event_has_tx(config)) {
            throw std::invalid_argument("ESP I2S endpoint has no physical TX event source");
        }
        break;
    case EspI2sDmaEvent::received:
    case EspI2sDmaEvent::receive_overflow:
        if (!event_has_rx(config)) {
            throw std::invalid_argument("ESP I2S endpoint has no physical RX event source");
        }
        break;
    default:
        throw std::invalid_argument("unknown ESP I2S DMA event source");
    }
}

[[nodiscard]] std::array<gpio_num_t, 5> reserved_i2s_pins(
    const EspI2sEndpointConfig& config) noexcept {
    if (config.role == EspPcmRole::pdm_receive) {
        return {config.pdm_clock_pin, config.pdm_data_pin,
                GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC};
    }
    return {config.standard_gpio.mclk, config.standard_gpio.bclk,
            config.standard_gpio.ws, config.standard_gpio.dout,
            config.standard_gpio.din};
}

struct OperationResult {
    IoStatus status = IoStatus::invalid_argument;
    DeviceResponse response = EmptyResponse{};
};

} // namespace

i2s_std_clk_config_t esp_i2s_default_standard_clock() noexcept {
    i2s_std_clk_config_t result = I2S_STD_CLK_DEFAULT_CONFIG(48000);
    result.mclk_multiple = I2S_MCLK_MULTIPLE_384;
    return result;
}

i2s_std_slot_config_t esp_i2s_default_standard_slots() noexcept {
    return I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
}

i2s_chan_config_t esp_i2s_default_channel_config() noexcept {
    return I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
}

void validate_esp_idf_i2s_config(const EspI2sEndpointConfig& config) {
    if (config.logical_name.empty() || config.logical_name.size() > kMaximumLogicalNameBytes) {
        throw std::invalid_argument("ESP I2S logical names must contain 1..63 bytes");
    }
    if (config.role != EspPcmRole::transmit && config.role != EspPcmRole::receive &&
        config.role != EspPcmRole::pdm_receive) {
        throw std::invalid_argument("unknown ESP PCM role");
    }
    if (config.channel.id < 0 || config.channel.id > I2S_NUM_2 ||
        (config.channel.role != I2S_ROLE_MASTER && config.channel.role != I2S_ROLE_SLAVE) ||
        config.channel.dma_desc_num < 2 || config.channel.dma_frame_num == 0 ||
        config.channel.tx_destination != I2S_DESTINATION_DMA ||
        config.channel.rx_destination != I2S_DESTINATION_DMA ||
        (config.full_duplex && config.role == EspPcmRole::pdm_receive)) {
        throw std::invalid_argument("ESP I2S controller, DMA or duplex policy is invalid");
    }
    if (config.allowed_formats.empty() || config.minimum_frame_rate_hz == 0 ||
        config.minimum_frame_rate_hz > config.maximum_frame_rate_hz ||
        config.maximum_frames_per_block == 0 || config.maximum_block_bytes == 0 ||
        config.maximum_pending_operations == 0 || config.io_timeout_ms == 0 ||
        config.worker_stack_bytes < 3072 ||
        (config.channel_count_mask & ~0x06U) != 0 || config.channel_count_mask == 0) {
        throw std::invalid_argument("ESP I2S format bounds or worker settings are invalid");
    }
    std::uint8_t formats = 0;
    for (const PcmSampleFormat format : config.allowed_formats) {
        const auto raw = static_cast<std::uint8_t>(format);
        if (raw > static_cast<std::uint8_t>(PcmSampleFormat::float_32_le)) {
            throw std::invalid_argument("unknown PCM sample format in owner allowlist");
        }
        if (format == PcmSampleFormat::float_32_le) {
            throw std::invalid_argument("ESP I2S float PCM is intentionally unsupported");
        }
        formats |= static_cast<std::uint8_t>(1U << raw);
    }
    if ((formats & kAllSignedFormats) == 0) {
        throw std::invalid_argument("ESP I2S allowlist needs a signed integer format");
    }
    if (config.role == EspPcmRole::pdm_receive &&
        formats != (1U << static_cast<std::uint8_t>(PcmSampleFormat::signed_16_le))) {
        throw std::invalid_argument("ESP PDM receive accepts only signed 16-bit PCM");
    }
#if CONFIG_IDF_TARGET_ESP32
    if ((formats & (1U << static_cast<std::uint8_t>(PcmSampleFormat::signed_24_le))) != 0) {
        throw std::invalid_argument("ESP32 24-bit I2S buffers are not packed signed_24_le");
    }
#endif
    if ((formats & (1U << static_cast<std::uint8_t>(PcmSampleFormat::signed_24_le))) != 0 &&
        config.channel.dma_frame_num % 3U != 0) {
        throw std::invalid_argument("24-bit ESP I2S requires dma_frame_num divisible by three");
    }
    if (!valid_std_slot_width(config.standard_slots.slot_bit_width) ||
        (config.full_duplex &&
         (!valid_std_slot_width(config.duplex_slots.slot_bit_width) ||
          config.standard_slots.slot_bit_width != config.duplex_slots.slot_bit_width))) {
        throw std::invalid_argument("ESP I2S standard and duplex slot widths are incompatible");
    }
#if SOC_I2S_HW_VERSION_2
    if (config.standard_slots.big_endian || config.standard_slots.bit_order_lsb ||
        (config.full_duplex &&
         (config.duplex_slots.big_endian || config.duplex_slots.bit_order_lsb))) {
        throw std::invalid_argument("ESP PCM requires little-endian MSB-first I2S buffer ordering");
    }
#endif
    if (config.role == EspPcmRole::pdm_receive) {
#if !SOC_I2S_SUPPORTS_PDM_RX
        throw std::invalid_argument("selected target has no I2S PDM receive mode");
#else
        if (!GPIO_IS_VALID_GPIO(config.pdm_data_pin) ||
            (config.channel.role == I2S_ROLE_MASTER
                 ? !GPIO_IS_VALID_OUTPUT_GPIO(config.pdm_clock_pin)
                 : !GPIO_IS_VALID_GPIO(config.pdm_clock_pin)) ||
            config.pdm_clock_pin == config.pdm_data_pin ||
            config.pdm_clock.bclk_div < 8 || config.pdm_clock.mclk_multiple == 0 ||
            std::popcount(static_cast<unsigned>(config.pdm_slot_mask)) == 0 ||
            std::popcount(static_cast<unsigned>(config.pdm_slot_mask)) > 2 ||
            (config.pdm_slot_mask != I2S_PDM_SLOT_LEFT &&
             config.pdm_slot_mask != I2S_PDM_SLOT_RIGHT &&
             config.pdm_slot_mask != I2S_PDM_SLOT_BOTH)) {
            throw std::invalid_argument("ESP PDM pins, slots or clock policy is invalid");
        }
#endif
    } else {
        validate_standard_pins(config);
    }
    validate_event_config(config);
}

struct EspIdfI2sBackend::State final {
    enum class WorkState : std::uint8_t { free, queued, running };
    struct Work {
        BackendOperation operation{};
        std::weak_ptr<PeripheralCompletionSink> completions;
        WorkState state = WorkState::free;
        bool event = false;
        std::atomic<bool> cancelled{false};
        std::atomic<bool> retired{false};
        std::uint8_t* tx = nullptr;
        std::uint8_t* rx = nullptr;
        ~Work() {
            heap_caps_free(tx);
            heap_caps_free(rx);
        }
    };

    struct Lease {
        bool open = false;
        bool closing = false;
        bool release_pending = false;
        std::uint64_t generation = 0;
        DeviceHandle device{};
    };
    struct EventLease : Lease {
        bool enabled = false;
        std::uint64_t committed_count = 0;
        std::uint64_t run_base = 0;
    };
    struct Channel {
        i2s_chan_handle_t handle = nullptr;
        bool callbacks = false;
        bool enabled = false;
    };

    explicit State(const EspI2sEndpointConfig& configuration)
        : config(configuration), name(configuration.logical_name),
          event_name(configuration.event_logical_name) {
        validate_esp_idf_i2s_config(config);
        for (PcmSampleFormat format : config.allowed_formats) {
            allowed_formats |= static_cast<std::uint8_t>(
                1U << static_cast<std::uint8_t>(format));
        }
        try {
            allocate_work();
            const esp_err_t channel_error = create_channels();
            if (channel_error != ESP_OK) construction_error("creating ESP I2S channels", channel_error);
            done = xSemaphoreCreateBinary();
            if (done == nullptr) throw std::runtime_error("creating ESP I2S completion semaphore failed");
            const auto result = xTaskCreate(
                &State::worker_entry, "mb_i2s", config.worker_stack_bytes, this,
                static_cast<UBaseType_t>(config.worker_priority), &worker);
            if (result != pdPASS) throw std::runtime_error("creating ESP I2S worker failed");
            worker_started = true;
        } catch (...) {
            (void)destroy_channels();
            if (done != nullptr) {
                vSemaphoreDelete(done);
                done = nullptr;
            }
            records.clear();
            throw;
        }
    }

    ~State() = default;

    [[nodiscard]] std::span<const DeviceOperation> operation_list() const noexcept {
        if (config.role == EspPcmRole::transmit) {
            return config.full_duplex ? std::span<const DeviceOperation>(kTxDuplexOperationsWithCancel)
                                      : std::span<const DeviceOperation>(kTxOperationsWithCancel);
        }
        if (config.role == EspPcmRole::receive) {
            return config.full_duplex ? std::span<const DeviceOperation>(kRxDuplexOperationsWithCancel)
                                      : std::span<const DeviceOperation>(kRxOperationsWithCancel);
        }
        return kRxOperationsWithCancel;
    }

    [[nodiscard]] Lease& lease_for(bool event) noexcept {
        return event ? static_cast<Lease&>(event_lease) : pcm_lease;
    }
    [[nodiscard]] const Lease& lease_for(bool event) const noexcept {
        return event ? static_cast<const Lease&>(event_lease) : pcm_lease;
    }

    [[nodiscard]] esp_err_t create_channels() noexcept {
        if (tx.handle != nullptr || rx.handle != nullptr) return ESP_ERR_INVALID_STATE;
        i2s_chan_handle_t new_tx = nullptr;
        i2s_chan_handle_t new_rx = nullptr;
        i2s_chan_handle_t* const tx_result = event_has_tx(config) ? &new_tx : nullptr;
        i2s_chan_handle_t* const rx_result = event_has_rx(config) ? &new_rx : nullptr;
        const esp_err_t error = i2s_new_channel(&config.channel, tx_result, rx_result);
        if (error == ESP_OK) {
            tx.handle = new_tx;
            rx.handle = new_rx;
        }
        return error;
    }

    void allocate_work() {
        records.reserve(config.maximum_pending_operations);
        queue.resize(config.maximum_pending_operations);
        for (std::size_t index = 0; index < config.maximum_pending_operations; ++index) {
            auto work = std::make_unique<Work>();
            if (event_has_tx(config)) {
                work->tx = static_cast<std::uint8_t*>(heap_caps_malloc(
                    config.maximum_block_bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
            }
            if (event_has_rx(config)) {
                work->rx = static_cast<std::uint8_t*>(heap_caps_malloc(
                    config.maximum_block_bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
            }
            if ((event_has_tx(config) && work->tx == nullptr) ||
                (event_has_rx(config) && work->rx == nullptr)) {
                throw std::runtime_error("allocating ESP I2S DMA staging blocks failed");
            }
            records.push_back(std::move(work));
        }
    }

    [[nodiscard]] std::uint64_t raw_event_count() const noexcept {
        portENTER_CRITICAL(&event_mux);
        const std::uint64_t result = event_raw_count;
        portEXIT_CRITICAL(&event_mux);
        return result;
    }

    void IRAM_ATTR note_dma_event() noexcept {
        portENTER_CRITICAL_ISR(&event_mux);
        if (event_raw_count != std::numeric_limits<std::uint64_t>::max()) {
            ++event_raw_count;
        }
        portEXIT_CRITICAL_ISR(&event_mux);
    }

    static bool IRAM_ATTR on_sent(
        i2s_chan_handle_t, i2s_event_data_t*, void* context) noexcept {
        static_cast<State*>(context)->note_dma_event();
        return false;
    }
    static bool IRAM_ATTR on_received(
        i2s_chan_handle_t, i2s_event_data_t*, void* context) noexcept {
        static_cast<State*>(context)->note_dma_event();
        return false;
    }

    [[nodiscard]] static std::uint64_t saturated_count(std::uint64_t value) noexcept {
        return std::min<std::uint64_t>(
            value, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
    }

    [[nodiscard]] std::uint64_t event_count() const noexcept {
        if (!event_lease.enabled) return saturated_count(event_lease.committed_count);
        const std::uint64_t raw = raw_event_count();
        const std::uint64_t running = raw >= event_lease.run_base
                                          ? raw - event_lease.run_base
                                          : 0;
        const std::uint64_t total = running >
                std::numeric_limits<std::uint64_t>::max() - event_lease.committed_count
            ? std::numeric_limits<std::uint64_t>::max()
            : event_lease.committed_count + running;
        return saturated_count(total);
    }

    void stop_event_capture() noexcept {
        if (!event_lease.enabled) return;
        event_lease.committed_count = event_count();
        const std::uint64_t raw = raw_event_count();
        event_lease.run_base = raw;
        event_lease.enabled = false;
    }

    [[nodiscard]] esp_err_t disable_channel(Channel& channel) noexcept {
        if (channel.handle == nullptr || !channel.enabled) return ESP_OK;
        const esp_err_t error = i2s_channel_disable(channel.handle);
        if (error == ESP_OK) channel.enabled = false;
        return error;
    }

    [[nodiscard]] esp_err_t destroy_channel(Channel& channel) noexcept {
        if (channel.handle == nullptr) return ESP_OK;
        esp_err_t error = disable_channel(channel);
        if (error == ESP_OK && channel.callbacks) {
            const i2s_event_callbacks_t none{};
            error = i2s_channel_register_event_callback(channel.handle, &none, this);
            if (error == ESP_OK) channel.callbacks = false;
        }
        if (error == ESP_OK) {
            error = i2s_del_channel(channel.handle);
            if (error == ESP_OK) {
                channel.handle = nullptr;
            }
        }
        if (error != ESP_OK) {
            ESP_LOGE(kLogTag, "destroying ESP I2S channel: %s", esp_err_to_name(error));
        }
        return error;
    }

    [[nodiscard]] OperationResult destroy_channels() noexcept {
        const esp_err_t tx_error = destroy_channel(tx);
        const esp_err_t rx_error = destroy_channel(rx);
        configured = false;
        started = false;
        maximum_frames = 0;
        frame_bytes = 0;
        if (tx.handle != nullptr || rx.handle != nullptr) {
            std::lock_guard lock(mutex);
            unavailable = true;
        }
        return {i2s_status(tx_error != ESP_OK ? tx_error : rx_error), EmptyResponse{}};
    }

    [[nodiscard]] esp_err_t init_channel(Channel& channel, bool transmit,
                                         const PcmConfigureRequest& request) {
        const i2s_slot_mode_t slot_mode = request.format.channel_count == 1
            ? static_cast<i2s_slot_mode_t>(I2S_SLOT_MODE_MONO)
            : static_cast<i2s_slot_mode_t>(I2S_SLOT_MODE_STEREO);
        if (config.role == EspPcmRole::pdm_receive) {
#if SOC_I2S_SUPPORTS_PDM_RX
            i2s_pdm_rx_config_t pdm{};
            pdm.clk_cfg = config.pdm_clock;
            pdm.clk_cfg.sample_rate_hz = request.format.frame_rate_hz;
            pdm.slot_cfg = I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(
                I2S_DATA_BIT_WIDTH_16BIT, slot_mode);
            pdm.slot_cfg.slot_mask = config.pdm_slot_mask;
            pdm.gpio_cfg.clk = config.pdm_clock_pin;
            pdm.gpio_cfg.din = config.pdm_data_pin;
            esp_err_t error = i2s_channel_init_pdm_rx_mode(channel.handle, &pdm);
            if (error != ESP_OK) return error;
#else
            return ESP_ERR_NOT_SUPPORTED;
#endif
        } else {
            i2s_std_config_t standard{};
            standard.clk_cfg = config.standard_clock;
            standard.clk_cfg.sample_rate_hz = request.format.frame_rate_hz;
            const bool primary_direction = transmit == (config.role == EspPcmRole::transmit);
            standard.slot_cfg = primary_direction ? config.standard_slots : config.duplex_slots;
            standard.slot_cfg.data_bit_width = data_width(request.format.sample_format);
            standard.slot_cfg.slot_mode = slot_mode;
            standard.gpio_cfg = config.standard_gpio;
            esp_err_t error = i2s_channel_init_std_mode(channel.handle, &standard);
            if (error != ESP_OK) return error;
        }
        return ESP_OK;
    }

    [[nodiscard]] esp_err_t register_callbacks() {
        if (!config.event_logical_name.empty()) {
            i2s_event_callbacks_t tx_callbacks{};
            i2s_event_callbacks_t rx_callbacks{};
            switch (config.event) {
            case EspI2sDmaEvent::sent:
                tx_callbacks.on_sent = &State::on_sent;
                break;
            case EspI2sDmaEvent::received:
                rx_callbacks.on_recv = &State::on_received;
                break;
            case EspI2sDmaEvent::receive_overflow:
                rx_callbacks.on_recv_q_ovf = &State::on_received;
                break;
            case EspI2sDmaEvent::send_overflow:
                tx_callbacks.on_send_q_ovf = &State::on_sent;
                break;
            }
            if (tx.handle != nullptr) {
                const esp_err_t error = i2s_channel_register_event_callback(
                    tx.handle, &tx_callbacks, this);
                if (error != ESP_OK) return error;
                tx.callbacks = true;
            }
            if (rx.handle != nullptr) {
                const esp_err_t error = i2s_channel_register_event_callback(
                    rx.handle, &rx_callbacks, this);
                if (error != ESP_OK) return error;
                rx.callbacks = true;
            }
        }
        return ESP_OK;
    }
    [[nodiscard]] bool event_callbacks_ready() const noexcept {
        switch (config.event) {
        case EspI2sDmaEvent::sent:
        case EspI2sDmaEvent::send_overflow:
            return tx.callbacks;
        case EspI2sDmaEvent::received:
        case EspI2sDmaEvent::receive_overflow:
            return rx.callbacks;
        }
        return false;
    }

    [[nodiscard]] std::optional<PcmFormat> actual_format(Channel& channel) const noexcept {
        i2s_chan_info_t info{};
        if (channel.handle == nullptr || i2s_channel_get_info(channel.handle, &info) != ESP_OK ||
            info.mode_cfg == nullptr || info.mclk_hz == 0 || info.bclk_hz == 0) {
            return std::nullopt;
        }
        std::uint64_t divisor = 0;
        if (config.role == EspPcmRole::pdm_receive) {
#if SOC_I2S_SUPPORTS_PDM_RX
            const auto& pdm = *static_cast<const i2s_pdm_rx_config_t*>(info.mode_cfg);
            if (pdm.clk_cfg.sample_rate_hz == 0) return std::nullopt;
            const std::uint64_t downsample = info.bclk_hz / pdm.clk_cfg.sample_rate_hz;
            divisor = downsample * pdm.clk_cfg.bclk_div;
#else
            return std::nullopt;
#endif
        } else {
            const auto& standard = *static_cast<const i2s_std_config_t*>(info.mode_cfg);
            divisor = standard.clk_cfg.mclk_multiple;
        }
        if (divisor == 0) return std::nullopt;
        const std::uint64_t rounded =
            (static_cast<std::uint64_t>(info.mclk_hz) + divisor / 2U) / divisor;
        if (rounded == 0 || rounded > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
        PcmFormat result = configured_request.format;
        result.frame_rate_hz = static_cast<std::uint32_t>(rounded);
        return result;
    }

    [[nodiscard]] OperationResult configure_endpoint(Work& work) {
        const BackendOperation& operation = work.operation;
        const IoStatus lease_status = bind_lease(operation, false);
        if (lease_status != IoStatus::completed) return {lease_status, EmptyResponse{}};
        if (started) return {IoStatus::busy, EmptyResponse{}};
        const auto& request = std::get<PcmConfigureRequest>(operation.request);
        configured_request = request;
        (void)destroy_channels();
        {
            std::lock_guard lock(mutex);
            if (unavailable) return {IoStatus::io_failure, EmptyResponse{}};
        }
        const esp_err_t channel_error = create_channels();
        if (channel_error != ESP_OK) return {i2s_status(channel_error), EmptyResponse{}};
        Channel& primary = config.role == EspPcmRole::transmit ? tx : rx;
        esp_err_t error = init_channel(primary, config.role == EspPcmRole::transmit, request);
        if (error == ESP_OK && config.full_duplex) {
            Channel& secondary = config.role == EspPcmRole::transmit ? rx : tx;
            error = init_channel(secondary, config.role != EspPcmRole::transmit, request);
        }
        if (error == ESP_OK) error = register_callbacks();
        if (error != ESP_OK) {
            (void)destroy_channels();
            return {i2s_status(error), EmptyResponse{}};
        }
        const auto actual = actual_format(primary);
        if (!actual || actual->frame_rate_hz < config.minimum_frame_rate_hz ||
            actual->frame_rate_hz > config.maximum_frame_rate_hz) {
            (void)destroy_channels();
            return {IoStatus::io_failure, EmptyResponse{}};
        }
        configured = true;
        maximum_frames = request.maximum_frames_per_block;
        frame_bytes = static_cast<std::size_t>(request.format.channel_count) *
                      sample_bytes(request.format.sample_format);
        sequence = 0;
        sequence_exhausted = false;
        return {IoStatus::completed, PcmConfigureResponse{*actual}};
    }

    [[nodiscard]] OperationResult start_stream(const BackendOperation& operation) {
        const IoStatus lease_status = bind_lease(operation, false);
        if (lease_status != IoStatus::completed) return {lease_status, EmptyResponse{}};
        if (!configured || started) return {IoStatus::busy, EmptyResponse{}};
        esp_err_t first_error = ESP_OK;
        if (event_has_tx(config) && !tx.enabled) {
            const esp_err_t error = i2s_channel_enable(tx.handle);
            if (error == ESP_OK) tx.enabled = true;
            else first_error = error;
        }
        if (first_error == ESP_OK && event_has_rx(config) && !rx.enabled) {
            const esp_err_t error = i2s_channel_enable(rx.handle);
            if (error == ESP_OK) rx.enabled = true;
            else first_error = error;
        }
        if (first_error != ESP_OK) {
            (void)disable_channel(tx);
            (void)disable_channel(rx);
            return {i2s_status(first_error), EmptyResponse{}};
        }
        started = true;
        return {IoStatus::completed, EmptyResponse{}};
    }

    [[nodiscard]] OperationResult stop_stream(const BackendOperation& operation) {
        const IoStatus lease_status = bind_lease(operation, false);
        if (lease_status != IoStatus::completed) return {lease_status, EmptyResponse{}};
        if (!started) return {IoStatus::busy, EmptyResponse{}};
        const esp_err_t tx_error = disable_channel(tx);
        const esp_err_t rx_error = disable_channel(rx);
        started = false;
        if (tx_error != ESP_OK || rx_error != ESP_OK) {
            std::lock_guard lock(mutex);
            unavailable = true;
        }
        return {i2s_status(tx_error != ESP_OK ? tx_error : rx_error), EmptyResponse{}};
    }

    [[nodiscard]] OperationResult write_frames(Work& work) {
        const auto& request = std::get<PcmWriteRequest>(work.operation.request);
        if (!configured || !started || tx.handle == nullptr) {
            return {IoStatus::busy, EmptyResponse{}};
        }
        const std::size_t bytes = request.frames->size();
        std::memcpy(work.tx, request.frames->data(), bytes);
        std::size_t written = 0;
        const esp_err_t error = i2s_channel_write(
            tx.handle, work.tx, bytes, &written, config.io_timeout_ms);
        if (error != ESP_OK) return {i2s_status(error), EmptyResponse{}};
        if (written == 0 || written % frame_bytes != 0) {
            return {IoStatus::io_failure, EmptyResponse{}};
        }
        return {IoStatus::completed, PcmWriteResponse{written / frame_bytes}};
    }

    [[nodiscard]] OperationResult read_frames(Work& work, std::uint32_t frames) {
        if (sequence_exhausted) return {IoStatus::io_failure, EmptyResponse{}};
        const std::size_t bytes = static_cast<std::size_t>(frames) * frame_bytes;
        std::size_t read = 0;
        const esp_err_t error = i2s_channel_read(
            rx.handle, work.rx, bytes, &read, config.io_timeout_ms);
        if (error != ESP_OK) return {i2s_status(error), EmptyResponse{}};
        if (read == 0 || read % frame_bytes != 0) {
            return {IoStatus::io_failure, EmptyResponse{}};
        }
        auto result = make_immutable_bytes(
            std::span<const std::uint8_t>(work.rx, read));
        const std::int64_t current = static_cast<std::int64_t>(sequence);
        if (sequence == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            sequence_exhausted = true;
        } else {
            ++sequence;
        }
        return {IoStatus::completed,
                PcmReadResponse{std::move(result), read / frame_bytes, current}};
    }

    [[nodiscard]] OperationResult transfer(Work& work) {
        const auto& request = std::get<PcmTransferRequest>(work.operation.request);
        if (!configured || !started || tx.handle == nullptr || rx.handle == nullptr) {
            return {IoStatus::busy, EmptyResponse{}};
        }
        const std::size_t bytes = request.transmit_frames->size();
        if (bytes != 0) {
            std::memcpy(work.tx, request.transmit_frames->data(), bytes);
            std::size_t written = 0;
            const esp_err_t error = i2s_channel_write(
                tx.handle, work.tx, bytes, &written, config.io_timeout_ms);
            if (error != ESP_OK) return {i2s_status(error), EmptyResponse{}};
            if (written != bytes) return {IoStatus::io_failure, EmptyResponse{}};
        }
        return read_frames(work, request.receive_frames);
    }

    [[nodiscard]] IoStatus bind_lease(const BackendOperation& operation, bool event) noexcept {
        std::lock_guard lock(mutex);
        Lease& lease = lease_for(event);
        if (!lease.open || lease.generation != operation.owner_generation) {
            return IoStatus::device_lost;
        }
        if (lease.device.is_empty()) {
            lease.device = operation.device;
        } else if (lease.device != operation.device) {
            return IoStatus::invalid_argument;
        }
        return IoStatus::completed;
    }

    [[nodiscard]] OperationResult open_endpoint(
        const BackendOperation& operation, bool event) {
        if (!event) {
            if (!unavailable && tx.handle == nullptr && rx.handle == nullptr) {
                const esp_err_t channel_error = create_channels();
                if (channel_error != ESP_OK) {
                    return {i2s_status(channel_error), EmptyResponse{}};
                }
            }
        }
        std::lock_guard lock(mutex);
        Lease& lease = lease_for(event);
        if ((!event && unavailable) || lease.open || lease.closing || lease.release_pending) {
            return {!event && unavailable ? IoStatus::io_failure : IoStatus::busy, EmptyResponse{}};
        }
        lease.open = true;
        lease.generation = operation.owner_generation;
        lease.device = DeviceHandle{};
        if (event) {
            event_lease.committed_count = 0;
            event_lease.run_base = raw_event_count();
            event_lease.enabled = false;
        }
        return {IoStatus::completed,
                OpenResponse{DeviceHandle{},
                             event ? esp_idf_i2s_event_link_version : version_of(config.role),
                             false}};
    }

    [[nodiscard]] OperationResult event_start(const BackendOperation& operation) {
        const IoStatus status = bind_lease(operation, true);
        if (status != IoStatus::completed) return {status, EmptyResponse{}};
        if (!configured || !event_callbacks_ready()) {
            return {IoStatus::busy, EmptyResponse{}};
        }
        if (event_lease.enabled) return {IoStatus::busy, EmptyResponse{}};
        const std::uint64_t raw = raw_event_count();
        event_lease.run_base = raw;
        event_lease.enabled = true;
        return {IoStatus::completed, EmptyResponse{}};
    }

    [[nodiscard]] OperationResult event_stop(const BackendOperation& operation) {
        const IoStatus status = bind_lease(operation, true);
        if (status != IoStatus::completed) return {status, EmptyResponse{}};
        std::lock_guard lock(mutex);
        if (!event_lease.enabled) return {IoStatus::busy, EmptyResponse{}};
        stop_event_capture();
        return {IoStatus::completed, EmptyResponse{}};
    }

    [[nodiscard]] OperationResult event_read(const BackendOperation& operation) {
        const IoStatus status = bind_lease(operation, true);
        if (status != IoStatus::completed) return {status, EmptyResponse{}};
        std::lock_guard lock(mutex);
        const std::uint64_t count = event_count();
        return {IoStatus::completed,
                EventLinkReadResponse{event_lease.enabled, true,
                                      static_cast<std::int64_t>(count)}};
    }

    [[nodiscard]] IoStatus validate_request(
        const BackendOperation& operation, bool event) const noexcept {
        const Capability capability = event ? Capability::event_link
                                            : capability_of(config.role);
        if (operation.owner_generation == 0 ||
            (event ? operation.logical_name != event_name : operation.logical_name != name)) {
            return IoStatus::invalid_argument;
        }
        const auto list = event ? std::span<const DeviceOperation>(kEventOperations)
                                : operation_list();
        if (!contains(list, operation.id) || operation.id == DeviceOperation::cancel) {
            return IoStatus::unsupported;
        }
        if (operation.id == DeviceOperation::open) {
            const auto* open = std::get_if<OpenRequest>(&operation.request);
            if (open == nullptr || !operation.device.is_empty()) {
                return IoStatus::invalid_argument;
            }
            if (open->minimum_version.capability != capability ||
                open->minimum_version.major != peripheral_abi_major) {
                return IoStatus::invalid_argument;
            }
            return open->minimum_version.minor == 0 ? IoStatus::completed : IoStatus::unsupported;
        }
        if (operation.device.is_empty() || operation.device.capability != capability ||
            operation.device.owner_generation != operation.owner_generation) {
            return IoStatus::invalid_argument;
        }
        switch (operation.id) {
        case DeviceOperation::configure: {
            const auto* request = std::get_if<PcmConfigureRequest>(&operation.request);
            if (request == nullptr || request->maximum_frames_per_block == 0 ||
                request->format.channel_count == 0 || request->format.frame_rate_hz == 0) {
                return IoStatus::invalid_argument;
            }
            const auto format = request->format.sample_format;
            if (format == PcmSampleFormat::float_32_le) return IoStatus::unsupported;
            const auto raw = static_cast<std::uint8_t>(format);
            if (raw > static_cast<std::uint8_t>(PcmSampleFormat::signed_32_le)) {
                return IoStatus::invalid_argument;
            }
            const std::uint8_t bit = static_cast<std::uint8_t>(1U << raw);
            if ((allowed_formats & bit) == 0) return IoStatus::permission_denied;
            if (config.role == EspPcmRole::pdm_receive &&
                format != PcmSampleFormat::signed_16_le) {
                return IoStatus::unsupported;
            }
            const auto channels = request->format.channel_count;
            if (channels > 2 || (config.channel_count_mask & (1U << channels)) == 0) {
                return IoStatus::permission_denied;
            }
            if (channels == 2 && config.role != EspPcmRole::pdm_receive &&
                (config.standard_slots.slot_mask != I2S_STD_SLOT_BOTH ||
                 (config.full_duplex && config.duplex_slots.slot_mask != I2S_STD_SLOT_BOTH))) {
                return IoStatus::permission_denied;
            }
            if (channels == 2 && config.role == EspPcmRole::pdm_receive) {
#if SOC_I2S_SUPPORTS_PDM_RX
                if (config.pdm_slot_mask != I2S_PDM_SLOT_BOTH) {
                    return IoStatus::permission_denied;
                }
#endif
            }
            if (request->format.frame_rate_hz < config.minimum_frame_rate_hz ||
                request->format.frame_rate_hz > config.maximum_frame_rate_hz) {
                return IoStatus::permission_denied;
            }
            if (request->maximum_frames_per_block > config.maximum_frames_per_block) {
                return IoStatus::permission_denied;
            }
            const std::uint64_t block_bytes =
                static_cast<std::uint64_t>(request->format.channel_count) *
                sample_bytes(format) * request->maximum_frames_per_block;
            if (block_bytes > config.maximum_block_bytes ||
                block_bytes > operation.budget.maximum_bytes_per_buffer) {
                return IoStatus::memory_exhausted;
            }
            if (config.role != EspPcmRole::pdm_receive) {
                const std::uint32_t requested_width = static_cast<std::uint32_t>(data_width(format));
                const std::uint32_t slot_width = std::max<std::uint32_t>(
                    requested_width,
                    static_cast<std::uint32_t>(config.standard_slots.slot_bit_width));
                const std::uint32_t frame_width = slot_width * 2U;
                if (static_cast<std::uint32_t>(config.standard_clock.mclk_multiple) == 0 ||
                    static_cast<std::uint32_t>(config.standard_clock.mclk_multiple) % frame_width != 0) {
                    return IoStatus::unsupported;
                }
            }
            return IoStatus::completed;
        }
        case DeviceOperation::write:
        case DeviceOperation::read:
        case DeviceOperation::transfer: {
            const ByteBufferPtr* bytes = nullptr;
            std::uint32_t frames = 0;
            if (operation.id == DeviceOperation::write) {
                const auto* request = std::get_if<PcmWriteRequest>(&operation.request);
                if (request == nullptr) return IoStatus::invalid_argument;
                bytes = &request->frames;
            } else if (operation.id == DeviceOperation::read) {
                const auto* request = std::get_if<PcmReadRequest>(&operation.request);
                if (request == nullptr) return IoStatus::invalid_argument;
                frames = request->frame_count;
            } else {
                const auto* request = std::get_if<PcmTransferRequest>(&operation.request);
                if (request == nullptr) return IoStatus::invalid_argument;
                bytes = &request->transmit_frames;
                frames = request->receive_frames;
            }
            if ((bytes != nullptr && *bytes == nullptr) ||
                (operation.id != DeviceOperation::write && frames == 0)) {
                return IoStatus::invalid_argument;
            }
            const std::size_t maximum_bytes = std::min<std::uint64_t>(
                config.maximum_block_bytes, operation.budget.maximum_bytes_per_buffer);
            if (bytes != nullptr && (*bytes)->size() > maximum_bytes) {
                return IoStatus::memory_exhausted;
            }
            if (frames != 0 && frames > config.maximum_frames_per_block) {
                return IoStatus::permission_denied;
            }
            if (operation.id == DeviceOperation::write && (*bytes)->empty()) {
                return IoStatus::invalid_argument;
            }
            return IoStatus::completed;
        }
        case DeviceOperation::start:
            return std::holds_alternative<StartRequest>(operation.request)
                       ? IoStatus::completed : IoStatus::invalid_argument;
        case DeviceOperation::stop:
            return std::holds_alternative<StopRequest>(operation.request)
                       ? IoStatus::completed : IoStatus::invalid_argument;
        case DeviceOperation::close:
            return std::holds_alternative<CloseRequest>(operation.request)
                       ? IoStatus::completed : IoStatus::invalid_argument;
        default:
            break;
        }
        return IoStatus::invalid_argument;
    }

    [[nodiscard]] IoStatus validate_data_against_configuration(
        const Work& work) const noexcept {
        if (!configured || !started) return IoStatus::busy;
        const auto& request = work.operation.request;
        std::size_t tx_bytes = 0;
        std::uint32_t rx_frames = 0;
        if (const auto* write = std::get_if<PcmWriteRequest>(&request)) {
            if (work.event || config.role != EspPcmRole::transmit) return IoStatus::unsupported;
            tx_bytes = write->frames->size();
        } else if (const auto* read = std::get_if<PcmReadRequest>(&request)) {
            if (work.event || config.role == EspPcmRole::transmit) return IoStatus::unsupported;
            rx_frames = read->frame_count;
        } else if (const auto* transfer = std::get_if<PcmTransferRequest>(&request)) {
            if (work.event || !config.full_duplex) return IoStatus::unsupported;
            tx_bytes = transfer->transmit_frames->size();
            rx_frames = transfer->receive_frames;
        }
        if (tx_bytes % frame_bytes != 0 || tx_bytes / frame_bytes > maximum_frames ||
            (rx_frames == 0 && work.operation.id != DeviceOperation::write)) {
            return IoStatus::invalid_argument;
        }
        if (rx_frames > maximum_frames) return IoStatus::permission_denied;
        return IoStatus::completed;
    }

    [[nodiscard]] IoStatus start(
        const BackendOperation& operation, bool event,
        std::weak_ptr<PeripheralCompletionSink> completions) {
        const IoStatus validation = validate_request(operation, event);
        if (validation != IoStatus::completed) return validation;
        std::unique_lock lock(mutex);
        if (!accepting) return IoStatus::device_lost;
        const std::size_t capacity = std::min<std::size_t>(
            records.size(), operation.budget.maximum_pending_operations);
        if (capacity == 0) return IoStatus::invalid_argument;
        if (pending_count >= capacity) return IoStatus::busy;
        Lease& lease = lease_for(event);
        if (operation.id == DeviceOperation::open) {
            if (!event && unavailable) return IoStatus::io_failure;
            if (lease.open || lease.closing || lease.release_pending) return IoStatus::busy;
        } else {
            if (!lease.open) {
                return lease.closing && lease.generation == operation.owner_generation
                           ? IoStatus::device_lost : IoStatus::invalid_argument;
            }
            if (lease.generation != operation.owner_generation ||
                (!lease.device.is_empty() && lease.device != operation.device)) {
                return IoStatus::invalid_argument;
            }
        }
        Work* selected = nullptr;
        for (auto& record : records) {
            if (record->state == WorkState::free) {
                selected = record.get();
                break;
            }
        }
        if (selected == nullptr) return IoStatus::busy;
        selected->operation = operation;
        selected->event = event;
        selected->completions = std::move(completions);
        selected->cancelled.store(false, std::memory_order_release);
        selected->retired.store(false, std::memory_order_release);
        selected->state = WorkState::queued;
        queue[(queue_head + queue_size) % queue.size()] = selected;
        ++queue_size;
        ++pending_count;
        if (operation.id == DeviceOperation::close) {
            if (event) stop_event_capture();
            lease.open = false;
            lease.closing = true;
            for (auto& record : records) {
                if (record.get() != selected && record->state != WorkState::free &&
                    record->event == event &&
                    record->operation.owner_generation == operation.owner_generation &&
                    record->operation.device == operation.device) {
                    record->cancelled.store(true, std::memory_order_release);
                    record->retired.store(true, std::memory_order_release);
                }
            }
        }
        lock.unlock();
        ready.notify_one();
        return IoStatus::completed;
    }

    [[nodiscard]] IoStatus cancel(const BackendOperation& operation) noexcept {
        std::lock_guard lock(mutex);
        for (auto& record : records) {
            if (record->state == WorkState::free ||
                record->operation.owner_generation != operation.owner_generation ||
                record->operation.operation != operation.operation) {
                continue;
            }
            if (record->cancelled.load(std::memory_order_acquire)) {
                return IoStatus::invalid_argument;
            }
            record->cancelled.store(true, std::memory_order_release);
            ready.notify_one();
            return IoStatus::completed;
        }
        return IoStatus::invalid_argument;
    }

    void release(std::uint64_t generation) noexcept {
        std::lock_guard lock(mutex);
        for (bool event : {false, true}) {
            Lease& lease = lease_for(event);
            if (lease.generation == generation && (lease.open || lease.closing)) {
                if (event) stop_event_capture();
                lease.open = false;
                lease.release_pending = true;
            }
        }
        for (auto& record : records) {
            if (record->state != WorkState::free &&
                record->operation.owner_generation == generation) {
                record->cancelled.store(true, std::memory_order_release);
                record->retired.store(true, std::memory_order_release);
            }
        }
        ready.notify_one();
    }

    [[nodiscard]] static bool cancellation_requested(const Work& work) noexcept {
        return work.cancelled.load(std::memory_order_acquire) ||
               work.retired.load(std::memory_order_acquire);
    }

    [[nodiscard]] OperationResult perform(Work& work) {
        const auto& operation = work.operation;
        if (operation.id == DeviceOperation::close) {
            return work.event ? OperationResult{IoStatus::completed, EmptyResponse{}}
                              : destroy_channels();
        }
        if (cancellation_requested(work)) return {IoStatus::cancelled, EmptyResponse{}};
        if (operation.id == DeviceOperation::open) return open_endpoint(operation, work.event);
        if (work.event) {
            switch (operation.id) {
            case DeviceOperation::start: return event_start(operation);
            case DeviceOperation::stop: return event_stop(operation);
            case DeviceOperation::read: return event_read(operation);
            default: return {IoStatus::unsupported, EmptyResponse{}};
            }
        }
        switch (operation.id) {
        case DeviceOperation::configure: return configure_endpoint(work);
        case DeviceOperation::start: return start_stream(operation);
        case DeviceOperation::stop: return stop_stream(operation);
        case DeviceOperation::write:
        case DeviceOperation::read:
        case DeviceOperation::transfer: {
            const IoStatus lease_status = bind_lease(operation, false);
            if (lease_status != IoStatus::completed) return {lease_status, EmptyResponse{}};
            const IoStatus valid = validate_data_against_configuration(work);
            if (valid != IoStatus::completed) return {valid, EmptyResponse{}};
            if (operation.id == DeviceOperation::write) return write_frames(work);
            if (operation.id == DeviceOperation::read) {
                return read_frames(work, std::get<PcmReadRequest>(operation.request).frame_count);
            }
            return transfer(work);
        }
        default: return {IoStatus::unsupported, EmptyResponse{}};
        }
    }

    void deliver(Work& work, IoStatus status, DeviceResponse response) noexcept {
        const auto sink = work.completions.lock();
        if (sink) sink->complete(work.operation, status, std::move(response));
    }

    void process(Work& work) {
        OperationResult result;
        try {
            result = perform(work);
        } catch (const std::bad_alloc&) {
            result = {IoStatus::memory_exhausted, EmptyResponse{}};
        }
        const bool cancelled = cancellation_requested(work);
        if (cancelled && work.operation.id == DeviceOperation::open &&
            result.status == IoStatus::completed) {
            if (!work.event) (void)destroy_channels();
            std::lock_guard lock(mutex);
            lease_for(work.event) = Lease{};
        }
        if (cancelled && !work.event && work.operation.id == DeviceOperation::configure &&
            result.status == IoStatus::completed) {
            (void)destroy_channels();
        }
        if (cancelled && !work.event && work.operation.id == DeviceOperation::start &&
            result.status == IoStatus::completed) {
            (void)disable_channel(tx);
            (void)disable_channel(rx);
            started = false;
        }
        if (cancelled && work.event && work.operation.id == DeviceOperation::start &&
            result.status == IoStatus::completed) {
            std::lock_guard lock(mutex);
            stop_event_capture();
        }
        if (cancelled) result.status = IoStatus::cancelled;
        deliver(work, result.status, std::move(result.response));
        {
            std::lock_guard lock(mutex);
            if (work.operation.id == DeviceOperation::close) {
                lease_for(work.event) = Lease{};
            }
            --pending_count;
            work.state = WorkState::free;
            work.operation = BackendOperation{};
            work.completions.reset();
            work.event = false;
        }
    }

    void process_release(bool event) noexcept {
        if (!event) (void)destroy_channels();
        std::lock_guard lock(mutex);
        lease_for(event) = Lease{};
    }

    void run() noexcept {
        for (;;) {
            Work* work = nullptr;
            bool release_pcm = false;
            bool release_event = false;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [&] {
                    return stopping || queue_size != 0 || pcm_lease.release_pending ||
                           event_lease.release_pending;
                });
                if (queue_size != 0) {
                    work = queue[queue_head];
                    queue_head = (queue_head + 1) % queue.size();
                    --queue_size;
                    work->state = WorkState::running;
                } else {
                    release_pcm = pcm_lease.release_pending;
                    release_event = event_lease.release_pending;
                    if (!release_pcm && !release_event && stopping) break;
                }
            }
            if (work != nullptr) {
                process(*work);
            } else {
                // Logical leases were revoked before the physical cleanup.
                if (release_pcm) process_release(false);
                if (release_event) process_release(true);
            }
        }
    }

    static void worker_entry(void* argument) {
        auto& state = *static_cast<State*>(argument);
        state.run();
        xSemaphoreGive(state.done);
        vTaskDelete(nullptr);
    }

    [[nodiscard]] bool stop() noexcept {
        if (stopped) return false;
        stopped = true;
        {
            std::lock_guard lock(mutex);
            accepting = false;
            stopping = true;
            stop_event_capture();
            for (auto& record : records) {
                if (record->state != WorkState::free) {
                    record->cancelled.store(true, std::memory_order_release);
                    record->retired.store(true, std::memory_order_release);
                }
            }
            ready.notify_all();
        }
        if (worker_started &&
            xSemaphoreTake(done, detail::worker_shutdown_ticks(config.io_timeout_ms)) != pdTRUE) {
            // A timed-out IDF read/write can still reference DMA staging and
            // its State. Retain both instead of destroying channels under it.
            ESP_LOGE(kLogTag, "I2S worker join timed out; retaining native resources");
            return false;
        }
        worker_started = false;
        (void)destroy_channels();
        if (done != nullptr) {
            vSemaphoreDelete(done);
            done = nullptr;
        }
        return true;
    }

    EspI2sEndpointConfig config;
    std::string name;
    std::string event_name;
    std::uint8_t allowed_formats = 0;
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<std::unique_ptr<Work>> records;
    std::vector<Work*> queue;
    std::size_t queue_head = 0;
    std::size_t queue_size = 0;
    std::size_t pending_count = 0;
    Lease pcm_lease;
    EventLease event_lease;
    mutable portMUX_TYPE event_mux = portMUX_INITIALIZER_UNLOCKED;
    std::uint64_t event_raw_count = 0;
    Channel tx;
    Channel rx;
    bool configured = false;
    bool started = false;
    bool unavailable = false;
    PcmConfigureRequest configured_request{};
    std::size_t maximum_frames = 0;
    std::size_t frame_bytes = 0;
    std::uint64_t sequence = 0;
    bool sequence_exhausted = false;
    bool accepting = true;
    bool stopping = false;
    bool stopped = false;
    SemaphoreHandle_t done = nullptr;
    TaskHandle_t worker = nullptr;
    bool worker_started = false;
};

EspIdfI2sBackend::EspIdfI2sBackend(const EspI2sEndpointConfig& config)
    : m_state(std::make_unique<State>(config)) {}
EspIdfI2sBackend::~EspIdfI2sBackend() {
    if (!m_state) return;
    if (m_state->stop()) m_state.reset();
    else (void)m_state.release();
}

std::span<const DeviceOperation> EspIdfI2sBackend::operations() const noexcept {
    return m_state->operation_list();
}

IoStatus EspIdfI2sBackend::start_operation(
    const BackendOperation& operation,
    std::weak_ptr<PeripheralCompletionSink> completions) {
    return m_state->start(operation, false, std::move(completions));
}

IoStatus EspIdfI2sBackend::cancel_operation(const BackendOperation& operation) {
    return m_state->cancel(operation);
}

void EspIdfI2sBackend::release_application(std::uint64_t owner_generation) {
    m_state->release(owner_generation);
}

EspIdfI2sDmaEventBackend::EspIdfI2sDmaEventBackend(
    std::shared_ptr<EspIdfI2sBackend> source)
    : m_source(std::move(source)) {
    if (m_source == nullptr || m_source->m_state->event_name.empty()) {
        throw std::invalid_argument("ESP I2S DMA event backend needs an event endpoint");
    }
}
EspIdfI2sDmaEventBackend::~EspIdfI2sDmaEventBackend() = default;

std::span<const DeviceOperation> EspIdfI2sDmaEventBackend::operations() const noexcept {
    return kEventOperations;
}

IoStatus EspIdfI2sDmaEventBackend::start_operation(
    const BackendOperation& operation,
    std::weak_ptr<PeripheralCompletionSink> completions) {
    return m_source->m_state->start(operation, true, std::move(completions));
}
IoStatus EspIdfI2sDmaEventBackend::cancel_operation(const BackendOperation& operation) {
    return m_source->m_state->cancel(operation);
}
void EspIdfI2sDmaEventBackend::release_application(std::uint64_t owner_generation) {
    m_source->m_state->release(owner_generation);
}

EspIdfI2sRegistry::EspIdfI2sRegistry(
    PeripheralRegistry& registry, std::span<const EspI2sEndpointConfig> endpoints)
    : m_registry(registry) {
    for (std::size_t index = 0; index < endpoints.size(); ++index) {
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (endpoints[previous].channel.id == endpoints[index].channel.id ||
                endpoints[previous].logical_name == endpoints[index].logical_name ||
                endpoints[previous].logical_name == endpoints[index].event_logical_name ||
                endpoints[previous].event_logical_name == endpoints[index].logical_name ||
                (!endpoints[previous].event_logical_name.empty() &&
                 endpoints[previous].event_logical_name == endpoints[index].event_logical_name)) {
                throw std::invalid_argument("duplicate ESP I2S controller or logical name");
            }
            const auto current_pins = reserved_i2s_pins(endpoints[index]);
            const auto previous_pins = reserved_i2s_pins(endpoints[previous]);
            bool duplicate_pin = false;
            for (gpio_num_t pin : current_pins) {
                if (pin == GPIO_NUM_NC) continue;
                duplicate_pin = duplicate_pin ||
                    std::find(previous_pins.begin(), previous_pins.end(), pin) != previous_pins.end();
            }
            if (duplicate_pin) {
                throw std::invalid_argument("duplicate ESP I2S physical GPIO reservation");
            }
        }
        validate_esp_idf_i2s_config(endpoints[index]);
        m_backends.push_back(std::make_shared<EspIdfI2sBackend>(endpoints[index]));
        const auto& config = endpoints[index];
        m_registry.register_endpoint(
            {config.logical_name, version_of(config.role), false},
            EndpointAccess::application, m_backends.back());
        if (!config.event_logical_name.empty()) {
            auto events = std::make_shared<EspIdfI2sDmaEventBackend>(m_backends.back());
            m_registry.register_endpoint(
                {config.event_logical_name, esp_idf_i2s_event_link_version, false},
                EndpointAccess::application, std::move(events));
        }
    }
}

} // namespace mb
