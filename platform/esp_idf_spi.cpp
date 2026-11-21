#include "esp_idf_spi.h"

#include "esp_idf_worker.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mb {
namespace {

constexpr char kLogTag[] = "mb_spi";
constexpr std::size_t kMaximumLogicalNameBytes = 63;
constexpr std::size_t kMaximumTransferBytes = 65'536;

constexpr std::array<DeviceOperation, 7> kOperations{
    DeviceOperation::open,
    DeviceOperation::configure,
    DeviceOperation::start,
    DeviceOperation::stop,
    DeviceOperation::transfer,
    DeviceOperation::close,
    DeviceOperation::cancel,
};

[[nodiscard]] IoStatus spi_status(esp_err_t error) noexcept {
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

void check_construction(esp_err_t error, std::string_view action) {
    if (error != ESP_OK) construction_error(action, error);
}

[[nodiscard]] CapabilityVersion version_of(const EspSpiEndpointConfig& config) noexcept {
    return config.capability == Capability::spi_multilane
               ? esp_idf_spi_multilane_version
               : esp_idf_spi_controller_version;
}

[[nodiscard]] bool supports(DeviceOperation id) noexcept {
    return std::find(kOperations.begin(), kOperations.end(), id) != kOperations.end() &&
           id != DeviceOperation::cancel;
}


[[nodiscard]] bool valid_output_or_unused(int pin) noexcept {
    return pin == GPIO_NUM_NC || GPIO_IS_VALID_OUTPUT_GPIO(pin);
}

void validate_pin_set(const EspSpiEndpointConfig& config) {
    const std::array<int, 9> signals{
        config.bus.mosi_io_num,
        config.bus.miso_io_num,
        config.bus.sclk_io_num,
        config.bus.quadwp_io_num,
        config.bus.quadhd_io_num,
        config.bus.data4_io_num,
        config.bus.data5_io_num,
        config.bus.data6_io_num,
        config.bus.data7_io_num,
    };
    if (!GPIO_IS_VALID_OUTPUT_GPIO(config.bus.sclk_io_num) ||
        !valid_output_or_unused(config.chip_select)) {
        throw std::invalid_argument("ESP SPI clock and chip select must be valid output GPIOs");
    }
    for (int pin : signals) {
        if (pin != GPIO_NUM_NC && !GPIO_IS_VALID_GPIO(pin)) {
            throw std::invalid_argument("ESP SPI owner configuration names an invalid GPIO");
        }
    }
    for (std::size_t index = 0; index < signals.size(); ++index) {
        if (signals[index] == GPIO_NUM_NC) continue;
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (signals[previous] == signals[index]) {
                throw std::invalid_argument("duplicate ESP SPI bus pin assignment");
            }
        }
        if (signals[index] == config.chip_select) {
            throw std::invalid_argument("ESP SPI chip select duplicates a bus pin");
        }
    }
    for (int pin = 5; pin <= 8; ++pin) {
        if (config.bus.iocfg[pin] != GPIO_NUM_NC) {
            throw std::invalid_argument("octal SPI data pins are outside the portable contract");
        }
    }
    const bool requires_data_out = config.full_duplex ||
        (static_cast<std::uint8_t>(config.supported_lanes) & 0x06U) != 0;
    if ((config.bus.mosi_io_num != GPIO_NUM_NC &&
         !GPIO_IS_VALID_OUTPUT_GPIO(config.bus.mosi_io_num)) ||
        (requires_data_out && !GPIO_IS_VALID_OUTPUT_GPIO(config.bus.mosi_io_num)) ||
        (config.full_duplex && !GPIO_IS_VALID_GPIO(config.bus.miso_io_num)) ||
        (config.bus.mosi_io_num == GPIO_NUM_NC && config.bus.miso_io_num == GPIO_NUM_NC)) {
        throw std::invalid_argument("ESP SPI needs data pins compatible with its duplex configuration");
    }
    const auto lanes = static_cast<std::uint8_t>(config.supported_lanes);
    if ((lanes & static_cast<std::uint8_t>(EspSpiLaneSupport::dual)) != 0 ||
        (lanes & static_cast<std::uint8_t>(EspSpiLaneSupport::quad)) != 0) {
        if (!GPIO_IS_VALID_OUTPUT_GPIO(config.bus.miso_io_num)) {
            throw std::invalid_argument("multilane SPI requires a bidirectional data1 GPIO");
        }
    }
    if ((lanes & static_cast<std::uint8_t>(EspSpiLaneSupport::quad)) != 0 &&
        (!GPIO_IS_VALID_OUTPUT_GPIO(config.bus.quadwp_io_num) ||
         !GPIO_IS_VALID_OUTPUT_GPIO(config.bus.quadhd_io_num))) {
        throw std::invalid_argument("quad SPI requires four bidirectional data GPIOs");
    }
}

struct OperationResult {
    IoStatus status = IoStatus::invalid_argument;
    DeviceResponse response = EmptyResponse{};
};

} // namespace

void validate_esp_idf_spi_config(const EspSpiEndpointConfig& config) {
    if (config.logical_name.empty() ||
        config.logical_name.size() > kMaximumLogicalNameBytes) {
        throw std::invalid_argument("ESP SPI logical names must contain 1..63 bytes");
    }
    if (config.capability != Capability::spi_controller &&
        config.capability != Capability::spi_multilane) {
        throw std::invalid_argument("ESP SPI endpoint must be a controller or multilane capability");
    }
    const int host = static_cast<int>(config.host);
    if (host < static_cast<int>(SPI2_HOST) || host >= SOC_SPI_PERIPH_NUM) {
        throw std::invalid_argument("ESP SPI host is invalid or reserved for SPI0/SPI1 flash");
    }
    const auto lanes = static_cast<std::uint8_t>(config.supported_lanes);
    if (lanes == 0 || (lanes & ~0x07U) != 0) {
        throw std::invalid_argument("ESP SPI configuration has unknown or absent lane support");
    }
    if (config.full_duplex &&
        lanes != static_cast<std::uint8_t>(EspSpiLaneSupport::single)) {
        throw std::invalid_argument("ESP-IDF full duplex is restricted to single-lane transfers");
    }
    if (config.capability == Capability::spi_controller &&
        lanes != static_cast<std::uint8_t>(EspSpiLaneSupport::single)) {
        throw std::invalid_argument("baseline SPI controller cannot advertise multilane support");
    }
    if (config.capability == Capability::spi_multilane && lanes == 1U) {
        throw std::invalid_argument("SPI multilane endpoint needs dual or quad lane support");
    }
    if (config.maximum_frequency_hz == 0 ||
        config.maximum_frequency_hz > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        config.maximum_transfer_bytes == 0 ||
        config.maximum_transfer_bytes > kMaximumTransferBytes ||
        config.maximum_pending_operations == 0 || config.io_timeout_ms == 0 ||
        config.allowed_modes == 0 || (config.allowed_modes & ~0x0fU) != 0 ||
        config.worker_stack_bytes < 3072) {
        throw std::invalid_argument("ESP SPI owner limits or worker settings are invalid");
    }
    if (config.cs_cycles_before_transfer > 16 || config.duty_cycle_pos == 0 ||
        config.duty_cycle_pos > 256 || config.input_delay_ns < 0) {
        throw std::invalid_argument("ESP SPI chip-select timing or duty cycle is invalid");
    }
    validate_pin_set(config);
}

struct EspIdfSpiBackend::State final {
    enum class WorkState : std::uint8_t { free, queued, running };
    struct Work {
        BackendOperation operation{};
        std::weak_ptr<PeripheralCompletionSink> completions;
        WorkState state = WorkState::free;
        std::atomic<bool> cancelled{false};
        std::atomic<bool> retired{false};
        void* tx = nullptr;
        void* rx = nullptr;

        ~Work() {
            std::free(tx);
            std::free(rx);
        }
    };

    struct Lease {
        bool open = false;
        bool closing = false;
        bool release_pending = false;
        std::uint64_t generation = 0;
        DeviceHandle device{};
    };

    explicit State(const EspSpiEndpointConfig& configuration)
        : config(configuration), name(configuration.logical_name) {
        validate_esp_idf_spi_config(config);
        config.bus.max_transfer_sz = static_cast<int>(config.maximum_transfer_bytes);
        config.bus.flags |= SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_SCLK;
        if (config.bus.mosi_io_num != GPIO_NUM_NC) config.bus.flags |= SPICOMMON_BUSFLAG_MOSI;
        if (config.bus.miso_io_num != GPIO_NUM_NC) config.bus.flags |= SPICOMMON_BUSFLAG_MISO;
        const auto lanes = static_cast<std::uint8_t>(config.supported_lanes);
        if ((lanes & 0x06U) != 0) config.bus.flags |= SPICOMMON_BUSFLAG_DUAL;
        if ((lanes & static_cast<std::uint8_t>(EspSpiLaneSupport::quad)) != 0) {
            config.bus.flags |= SPICOMMON_BUSFLAG_WPHD;
        }
        try {
            check_construction(
                spi_bus_initialize(config.host, &config.bus, config.dma_channel),
                "initializing ESP SPI bus");
            bus_initialized = true;
            allocate_work();
            done = xSemaphoreCreateBinary();
            if (done == nullptr) throw std::runtime_error("creating ESP SPI completion semaphore failed");
            const auto task = xTaskCreate(
                &State::worker_entry, "mb_spi", config.worker_stack_bytes, this,
                static_cast<UBaseType_t>(config.worker_priority), &worker);
            if (task != pdPASS) throw std::runtime_error("creating ESP SPI worker failed");
            worker_started = true;
        } catch (...) {
            cleanup_construction();
            throw;
        }
    }

    ~State() = default;

    void allocate_work() {
        records.reserve(config.maximum_pending_operations);
        queue.resize(config.maximum_pending_operations);
        for (std::size_t index = 0; index < config.maximum_pending_operations; ++index) {
            auto work = std::make_unique<Work>();
            work->tx = spi_bus_dma_memory_alloc(config.host, config.maximum_transfer_bytes, 0);
            work->rx = spi_bus_dma_memory_alloc(config.host, config.maximum_transfer_bytes, 0);
            if (work->tx == nullptr || work->rx == nullptr) {
                throw std::runtime_error("allocating ESP SPI DMA staging buffers failed");
            }
            records.push_back(std::move(work));
        }
    }

    void cleanup_construction() noexcept {
        if (done != nullptr) {
            vSemaphoreDelete(done);
            done = nullptr;
        }
        records.clear();
        if (bus_initialized) {
            (void)spi_bus_free(config.host);
            bus_initialized = false;
        }
    }

    [[nodiscard]] IoStatus validate_request(const BackendOperation& operation) const noexcept {
        if (operation.logical_name != name || operation.owner_generation == 0 ||
            !supports(operation.id)) {
            return IoStatus::invalid_argument;
        }
        if (operation.id == DeviceOperation::open) {
            const auto* request = std::get_if<OpenRequest>(&operation.request);
            if (request == nullptr || !operation.device.is_empty()) {
                return IoStatus::invalid_argument;
            }
            const auto minimum = request->minimum_version;
            if (minimum.capability != config.capability ||
                minimum.major != peripheral_abi_major) {
                return IoStatus::invalid_argument;
            }
            return minimum.minor == 0 ? IoStatus::completed : IoStatus::unsupported;
        }
        if (operation.device.is_empty() || operation.device.capability != config.capability ||
            operation.device.owner_generation != operation.owner_generation) {
            return IoStatus::invalid_argument;
        }
        switch (operation.id) {
        case DeviceOperation::configure: {
            const auto* request = std::get_if<SpiConfigureRequest>(&operation.request);
            if (request == nullptr || request->frequency_hz == 0) {
                return IoStatus::invalid_argument;
            }
            const auto mode = static_cast<std::uint8_t>(request->mode);
            if (mode > 3 || (config.allowed_modes & (1U << mode)) == 0) {
                return IoStatus::permission_denied;
            }
            const auto order = static_cast<std::uint8_t>(request->bit_order);
            if (order > 1) return IoStatus::invalid_argument;
            const auto lanes = static_cast<std::uint8_t>(request->lanes);
            if (lanes != 1 && lanes != 2 && lanes != 4) {
                return IoStatus::invalid_argument;
            }
            const auto lane_bit = lanes == 1
                                      ? static_cast<std::uint8_t>(EspSpiLaneSupport::single)
                                      : lanes == 2
                                            ? static_cast<std::uint8_t>(EspSpiLaneSupport::dual)
                                            : static_cast<std::uint8_t>(EspSpiLaneSupport::quad);
            if ((static_cast<std::uint8_t>(config.supported_lanes) & lane_bit) == 0) {
                return IoStatus::permission_denied;
            }
            return IoStatus::completed;
        }
        case DeviceOperation::transfer: {
            const auto* request = std::get_if<SpiTransferRequest>(&operation.request);
            if (request == nullptr || request->transmit == nullptr) {
                return IoStatus::invalid_argument;
            }
            const std::size_t tx_bytes = request->transmit->size();
            if (tx_bytes == 0 && request->receive_bytes == 0) {
                return IoStatus::invalid_argument;
            }
            const auto maximum = std::min<std::uint64_t>(
                config.maximum_transfer_bytes, operation.budget.maximum_bytes_per_buffer);
            if (tx_bytes > maximum || request->receive_bytes > maximum) {
                return IoStatus::memory_exhausted;
            }
            if (tx_bytes != 0 && config.bus.mosi_io_num == GPIO_NUM_NC) {
                return IoStatus::unsupported;
            }
            if (request->receive_bytes != 0 && config.bus.miso_io_num == GPIO_NUM_NC) {
                return IoStatus::unsupported;
            }
#if !SOC_SPI_HD_BOTH_INOUT_SUPPORTED
            if (!config.full_duplex && tx_bytes != 0 && request->receive_bytes != 0) {
                return IoStatus::unsupported;
            }
#endif
            return IoStatus::completed;
        }
        case DeviceOperation::start:
            return std::holds_alternative<StartRequest>(operation.request)
                       ? IoStatus::completed
                       : IoStatus::invalid_argument;
        case DeviceOperation::stop:
            return std::holds_alternative<StopRequest>(operation.request)
                       ? IoStatus::completed
                       : IoStatus::invalid_argument;
        case DeviceOperation::close:
            return std::holds_alternative<CloseRequest>(operation.request)
                       ? IoStatus::completed
                       : IoStatus::invalid_argument;
        default:
            return IoStatus::invalid_argument;
        }
    }

    [[nodiscard]] IoStatus start(
        const BackendOperation& operation,
        std::weak_ptr<PeripheralCompletionSink> completions) {
        const IoStatus validation = validate_request(operation);
        if (validation != IoStatus::completed) return validation;
        std::unique_lock lock(mutex);
        if (!accepting) return IoStatus::device_lost;
        const std::size_t capacity = std::min<std::size_t>(
            records.size(), operation.budget.maximum_pending_operations);
        if (capacity == 0) return IoStatus::invalid_argument;
        if (pending_count >= capacity) return IoStatus::busy;
        if (operation.id == DeviceOperation::open) {
            if (unavailable) return IoStatus::io_failure;
            if (lease.open || lease.closing || lease.release_pending) return IoStatus::busy;
        } else {
            if (!lease.open) {
                return lease.generation == operation.owner_generation && lease.closing
                           ? IoStatus::device_lost
                           : IoStatus::invalid_argument;
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
        selected->completions = std::move(completions);
        selected->cancelled.store(false, std::memory_order_release);
        selected->retired.store(false, std::memory_order_release);
        selected->state = WorkState::queued;
        queue[(queue_head + queue_size) % queue.size()] = selected;
        ++queue_size;
        ++pending_count;

        if (operation.id == DeviceOperation::close) {
            lease.open = false;
            lease.closing = true;
            for (auto& record : records) {
                if (record.get() != selected && record->state != WorkState::free &&
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
        if (lease.generation == generation && (lease.open || lease.closing)) {
            lease.open = false;
            lease.release_pending = true;
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

    [[nodiscard]] IoStatus bind_lease(const BackendOperation& operation) noexcept {
        std::lock_guard lock(mutex);
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

    [[nodiscard]] OperationResult open_endpoint(const BackendOperation& operation) {
        {
            std::lock_guard lock(mutex);
            if (unavailable || lease.closing || lease.release_pending || lease.open) {
                return {IoStatus::busy, EmptyResponse{}};
            }
            lease.open = true;
            lease.generation = operation.owner_generation;
            lease.device = DeviceHandle{};
        }
        return {IoStatus::completed,
                OpenResponse{DeviceHandle{}, version_of(config), false}};
    }

    [[nodiscard]] esp_err_t remove_device() noexcept {
        if (device_handle == nullptr) return ESP_OK;
        const esp_err_t result = spi_bus_remove_device(device_handle);
        if (result == ESP_OK) device_handle = nullptr;
        return result;
    }

    [[nodiscard]] OperationResult close_physical() noexcept {
        const esp_err_t error = remove_device();
        configured = false;
        started = false;
        if (error != ESP_OK) {
            std::lock_guard lock(mutex);
            unavailable = true;
        }
        return {spi_status(error), EmptyResponse{}};
    }

    [[nodiscard]] OperationResult configure_endpoint(const BackendOperation& operation) {
        const IoStatus lease_status = bind_lease(operation);
        if (lease_status != IoStatus::completed) return {lease_status, EmptyResponse{}};
        if (started) return {IoStatus::busy, EmptyResponse{}};
        const auto& request = std::get<SpiConfigureRequest>(operation.request);
        esp_err_t error = remove_device();
        if (error != ESP_OK) return {spi_status(error), EmptyResponse{}};
        configured = false;

        spi_device_interface_config_t device{};
        device.mode = static_cast<std::uint8_t>(request.mode);
        device.clock_source = config.clock_source;
        device.duty_cycle_pos = config.duty_cycle_pos;
        device.cs_ena_pretrans = config.cs_cycles_before_transfer;
        device.cs_ena_posttrans = config.cs_cycles_after_transfer;
        device.clock_speed_hz = static_cast<int>(std::min(
            request.frequency_hz, config.maximum_frequency_hz));
        device.input_delay_ns = config.input_delay_ns;
        device.sample_point = config.sample_point;
        device.spics_io_num = config.chip_select;
        device.flags = request.bit_order == BitOrder::least_significant_first
                           ? SPI_DEVICE_BIT_LSBFIRST
                           : 0;
        if (!config.full_duplex) device.flags |= SPI_DEVICE_HALFDUPLEX;
        if (config.cs_active_high) device.flags |= SPI_DEVICE_POSITIVE_CS;
        device.queue_size = 1;
        error = spi_bus_add_device(config.host, &device, &device_handle);
        if (error != ESP_OK) return {spi_status(error), EmptyResponse{}};
        int actual_khz = 0;
        error = spi_device_get_actual_freq(device_handle, &actual_khz);
        if (error != ESP_OK || actual_khz <= 0 ||
            static_cast<std::uint64_t>(actual_khz) * 1000U > config.maximum_frequency_hz) {
            const esp_err_t reported = error == ESP_OK ? ESP_ERR_NOT_SUPPORTED : error;
            (void)remove_device();
            return {spi_status(reported), EmptyResponse{}};
        }
        configuration = request;
        configuration.frequency_hz = std::min(request.frequency_hz,
                                              config.maximum_frequency_hz);
        configured = true;
        return {IoStatus::completed,
                SpiConfigureResponse{static_cast<std::uint32_t>(actual_khz) * 1000U}};
    }

    [[nodiscard]] OperationResult transfer(Work& work) {
        const BackendOperation& operation = work.operation;
        const IoStatus lease_status = bind_lease(operation);
        if (lease_status != IoStatus::completed) return {lease_status, EmptyResponse{}};
        if (!configured || !started || device_handle == nullptr) {
            return {IoStatus::busy, EmptyResponse{}};
        }
        const auto& request = std::get<SpiTransferRequest>(operation.request);
        const std::size_t tx_bytes = request.transmit->size();
        const std::size_t rx_bytes = request.receive_bytes;
        const std::size_t transferred = std::max(tx_bytes, rx_bytes);
        spi_transaction_t transaction{};
        transaction.rxlength = rx_bytes * 8U;
        if (config.full_duplex) {
            transaction.length = transferred * 8U;
            std::memset(work.tx, 0, transferred);
        } else {
            transaction.length = tx_bytes * 8U;
        }
        if (tx_bytes != 0 || (config.full_duplex && rx_bytes != 0)) {
            transaction.tx_buffer = work.tx;
        }
        if (tx_bytes != 0) std::memcpy(work.tx, request.transmit->data(), tx_bytes);
        if (rx_bytes != 0) transaction.rx_buffer = work.rx;
        if (configuration.lanes == SpiLaneCount::dual) {
            transaction.flags |= SPI_TRANS_MODE_DIO;
        } else if (configuration.lanes == SpiLaneCount::quad) {
            transaction.flags |= SPI_TRANS_MODE_QIO;
        }

        const TickType_t timeout = std::max<TickType_t>(1, pdMS_TO_TICKS(config.io_timeout_ms));
        esp_err_t error = spi_device_queue_trans(device_handle, &transaction, timeout);
        bool finished = false;
        if (error == ESP_OK) {
            spi_transaction_t* result = nullptr;
            error = spi_device_get_trans_result(device_handle, &result, timeout);
            if (error == ESP_OK && result == &transaction) {
                finished = true;
            } else if (error == ESP_OK) {
                error = ESP_ERR_INVALID_STATE;
            } else if (error == ESP_ERR_TIMEOUT) {
                // The timed-out hardware descriptor cannot be abandoned: drain
                // it before this staging buffer or device is used again.
                error = spi_device_get_trans_result(
                    device_handle, &result, portMAX_DELAY);
                if (error == ESP_OK && result == &transaction) {
                    finished = true;
                    error = ESP_ERR_TIMEOUT;
                } else if (error == ESP_OK) {
                    error = ESP_ERR_INVALID_STATE;
                }
            }
        }
        if (!finished || error != ESP_OK) {
            if (cancellation_requested(work)) return {IoStatus::cancelled, EmptyResponse{}};
            return {spi_status(error), EmptyResponse{}};
        }
        if ((transaction.flags & (SPI_TRANS_DMA_RX_FAIL | SPI_TRANS_DMA_TX_FAIL)) != 0) {
            return {IoStatus::io_failure, EmptyResponse{}};
        }
        auto received = make_immutable_bytes(
            std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(work.rx), rx_bytes));
        return {IoStatus::completed, SpiTransferResponse{std::move(received), transferred}};
    }

    [[nodiscard]] OperationResult perform(Work& work) {
        const auto& operation = work.operation;
        if (operation.id == DeviceOperation::close) return close_physical();
        if (cancellation_requested(work)) return {IoStatus::cancelled, EmptyResponse{}};
        switch (operation.id) {
        case DeviceOperation::open: return open_endpoint(operation);
        case DeviceOperation::configure: return configure_endpoint(operation);
        case DeviceOperation::transfer: return transfer(work);
        case DeviceOperation::start: {
            const IoStatus status = bind_lease(operation);
            if (status != IoStatus::completed) return {status, EmptyResponse{}};
            if (!configured || started) return {IoStatus::busy, EmptyResponse{}};
            started = true;
            return {IoStatus::completed, EmptyResponse{}};
        }
        case DeviceOperation::stop: {
            const IoStatus status = bind_lease(operation);
            if (status != IoStatus::completed) return {status, EmptyResponse{}};
            if (!started) return {IoStatus::busy, EmptyResponse{}};
            started = false;
            return {IoStatus::completed, EmptyResponse{}};
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
            std::lock_guard lock(mutex);
            lease = Lease{};
        }
        if (cancelled && work.operation.id == DeviceOperation::configure &&
            result.status == IoStatus::completed) {
            (void)close_physical();
        }
        if (cancelled && work.operation.id == DeviceOperation::start &&
            result.status == IoStatus::completed) {
            started = false;
        }
        if (cancelled) result.status = IoStatus::cancelled;
        deliver(work, result.status, std::move(result.response));
        {
            std::lock_guard lock(mutex);
            if (work.operation.id == DeviceOperation::close) {
                const bool failed = result.status != IoStatus::completed &&
                                    result.status != IoStatus::cancelled;
                lease = Lease{};
                if (failed) unavailable = true;
            }
            --pending_count;
            work.state = WorkState::free;
            work.operation = BackendOperation{};
            work.completions.reset();
        }
    }

    void process_release() noexcept {
        (void)close_physical();
        std::lock_guard lock(mutex);
        lease = Lease{};
    }

    void run() noexcept {
        for (;;) {
            Work* work = nullptr;
            bool release_requested = false;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [&] { return stopping || queue_size != 0 || lease.release_pending; });
                if (queue_size != 0) {
                    work = queue[queue_head];
                    queue_head = (queue_head + 1) % queue.size();
                    --queue_size;
                    work->state = WorkState::running;
                } else if (lease.release_pending) {
                    release_requested = true;
                } else if (stopping) {
                    break;
                }
            }
            if (work != nullptr) {
                process(*work);
            } else if (release_requested) {
                process_release();
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
            // The IDF SPI driver can still own a stack transaction and staging
            // buffers after its wait expires. Retain the complete state and bus
            // reservation rather than letting that worker access freed memory.
            ESP_LOGE(kLogTag, "SPI worker join timed out; retaining native resources");
            return false;
        }
        worker_started = false;
        (void)remove_device();
        if (bus_initialized) {
            const esp_err_t error = spi_bus_free(config.host);
            if (error != ESP_OK) {
                ESP_LOGE(kLogTag, "freeing ESP SPI bus: %s", esp_err_to_name(error));
            }
            bus_initialized = false;
        }
        if (done != nullptr) {
            vSemaphoreDelete(done);
            done = nullptr;
        }
        return true;
    }

    EspSpiEndpointConfig config;
    std::string name;
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<std::unique_ptr<Work>> records;
    std::vector<Work*> queue;
    std::size_t queue_head = 0;
    std::size_t queue_size = 0;
    std::size_t pending_count = 0;
    Lease lease;
    spi_device_handle_t device_handle = nullptr;
    SpiConfigureRequest configuration{};
    bool configured = false;
    bool started = false;
    bool unavailable = false;
    bool bus_initialized = false;
    bool accepting = true;
    bool stopping = false;
    bool stopped = false;
    SemaphoreHandle_t done = nullptr;
    TaskHandle_t worker = nullptr;
    bool worker_started = false;
};

EspIdfSpiBackend::EspIdfSpiBackend(const EspSpiEndpointConfig& config)
    : m_state(std::make_unique<State>(config)) {}
EspIdfSpiBackend::~EspIdfSpiBackend() {
    if (!m_state) return;
    if (m_state->stop()) m_state.reset();
    else (void)m_state.release();
}

std::span<const DeviceOperation> EspIdfSpiBackend::operations() const noexcept {
    return kOperations;
}

IoStatus EspIdfSpiBackend::start_operation(
    const BackendOperation& operation,
    std::weak_ptr<PeripheralCompletionSink> completions) {
    return m_state->start(operation, std::move(completions));
}

IoStatus EspIdfSpiBackend::cancel_operation(const BackendOperation& operation) {
    return m_state->cancel(operation);
}

void EspIdfSpiBackend::release_application(std::uint64_t owner_generation) {
    m_state->release(owner_generation);
}

EspIdfSpiRegistry::EspIdfSpiRegistry(
    PeripheralRegistry& registry, std::span<const EspSpiEndpointConfig> endpoints)
    : m_registry(registry) {
    for (std::size_t index = 0; index < endpoints.size(); ++index) {
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (endpoints[previous].logical_name == endpoints[index].logical_name ||
                endpoints[previous].host == endpoints[index].host) {
                throw std::invalid_argument("duplicate ESP SPI name or host reservation");
            }
            bool duplicate_pin = endpoints[previous].chip_select == endpoints[index].chip_select &&
                                 endpoints[index].chip_select != GPIO_NUM_NC;
            for (int current = 0; current < 9; ++current) {
                const int pin = endpoints[index].bus.iocfg[current];
                if (pin == GPIO_NUM_NC) continue;
                for (int earlier = 0; earlier < 9; ++earlier) {
                    duplicate_pin = duplicate_pin || pin == endpoints[previous].bus.iocfg[earlier];
                }
                duplicate_pin = duplicate_pin || pin == endpoints[previous].chip_select;
            }
            for (int earlier = 0; earlier < 9; ++earlier) {
                const int pin = endpoints[previous].bus.iocfg[earlier];
                duplicate_pin = duplicate_pin ||
                    (pin != GPIO_NUM_NC && pin == endpoints[index].chip_select);
            }
            if (duplicate_pin) {
                throw std::invalid_argument("duplicate ESP SPI physical GPIO reservation");
            }
        }
        m_backends.push_back(std::make_shared<EspIdfSpiBackend>(endpoints[index]));
        const auto& config = endpoints[index];
        m_registry.register_endpoint(
            {config.logical_name, version_of(config), false},
            EndpointAccess::application, m_backends.back());
    }
}

} // namespace mb
