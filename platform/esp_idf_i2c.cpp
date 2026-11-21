#include "esp_idf_i2c.h"

#include "esp_idf_worker.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
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

constexpr char kLogTag[] = "mb_i2c";
constexpr std::size_t kMaximumLogicalNameBytes = 63;
constexpr std::uint32_t kMaximumTransferBytes = 65'536;
constexpr std::uint32_t kMinimumFrequencyHz = 1000;
constexpr std::uint32_t kMaximumFrequencyHz = 400'000;
constexpr std::uint8_t kMaximumGlitchIgnoreCount = 7;

constexpr std::array<DeviceOperation, 9> kOperations{
    DeviceOperation::open,
    DeviceOperation::configure,
    DeviceOperation::start,
    DeviceOperation::stop,
    DeviceOperation::read,
    DeviceOperation::write,
    DeviceOperation::transfer,
    DeviceOperation::close,
    DeviceOperation::cancel,
};

struct OperationResult {
    IoStatus status = IoStatus::invalid_argument;
    DeviceResponse response = EmptyResponse{};
};

struct TransactionPlan {
    std::uint16_t address = 0;
    bool ten_bit_address = false;
    std::span<const std::uint8_t> write_bytes;
    std::size_t read_bytes = 0;
    bool repeated_start = false;
};

[[nodiscard]] IoStatus i2c_status(esp_err_t error) noexcept {
    switch (error) {
    case ESP_OK: return IoStatus::completed;
    case ESP_ERR_INVALID_ARG: return IoStatus::invalid_argument;
    case ESP_ERR_INVALID_STATE: return IoStatus::busy;
    case ESP_ERR_NOT_SUPPORTED: return IoStatus::unsupported;
    case ESP_ERR_TIMEOUT: return IoStatus::timeout;
    case ESP_ERR_INVALID_RESPONSE:
    case ESP_ERR_NOT_FOUND: return IoStatus::device_lost;
    case ESP_ERR_NO_MEM: return IoStatus::memory_exhausted;
    default: return IoStatus::io_failure;
    }
}

[[nodiscard]] I2cFault i2c_fault(esp_err_t error) noexcept {
    if (error == ESP_ERR_INVALID_RESPONSE || error == ESP_ERR_NOT_FOUND) {
        return I2cFault::nack;
    }
    // IDF reports its hardware arbitration interrupt through I2C_EVENT_TIMEOUT
    // and exposes ESP_ERR_TIMEOUT to this synchronous API.
    return error == ESP_ERR_TIMEOUT ? I2cFault::timeout : I2cFault::none;
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

[[nodiscard]] bool supports(DeviceOperation id) noexcept {
    return std::find(kOperations.begin(), kOperations.end(), id) != kOperations.end() &&
           id != DeviceOperation::cancel;
}

[[nodiscard]] bool valid_address_width(std::uint16_t address, bool ten_bit) noexcept {
    if (ten_bit) return address <= 0x03ffU;
    // General call, CBUS, HS-mode, 10-bit-prefix, and reserved seven-bit
    // encodings are not application peripheral addresses.
    return address >= 0x08U && address <= 0x77U;
}

void validate_address_range(const EspI2cAddressRange& range) {
#if !SOC_I2C_SUPPORT_10BIT_ADDR
    if (range.ten_bit) {
        throw std::invalid_argument("ESP I2C target does not support ten-bit addresses");
    }
#endif
    if (range.first_address > range.last_address ||
        !valid_address_width(range.first_address, range.ten_bit) ||
        !valid_address_width(range.last_address, range.ten_bit)) {
        throw std::invalid_argument("ESP I2C owner address range is invalid or reserved");
    }
}

[[nodiscard]] TransactionPlan read_plan(const I2cReadRequest& request) noexcept {
    return TransactionPlan{
        request.address,
        request.ten_bit_address,
        {},
        request.byte_count,
        false,
    };
}

[[nodiscard]] TransactionPlan write_plan(const I2cWriteRequest& request) noexcept {
    return TransactionPlan{
        request.address,
        request.ten_bit_address,
        std::span<const std::uint8_t>(*request.bytes),
        0,
        false,
    };
}

[[nodiscard]] TransactionPlan transfer_plan(const I2cTransferRequest& request) noexcept {
    return TransactionPlan{
        request.address,
        request.ten_bit_address,
        std::span<const std::uint8_t>(*request.write_bytes),
        request.read_bytes,
        request.repeated_start,
    };
}

} // namespace

void validate_esp_idf_i2c_config(const EspI2cEndpointConfig& config) {
    if (config.logical_name.empty() ||
        config.logical_name.size() > kMaximumLogicalNameBytes) {
        throw std::invalid_argument("ESP I2C logical names must contain 1..63 bytes");
    }
    if (config.port < 0 || config.port >= static_cast<i2c_port_num_t>(SOC_HP_I2C_NUM)) {
        throw std::invalid_argument(
            "ESP I2C port must name an explicit general-purpose HP controller");
    }
    if (!GPIO_IS_VALID_GPIO(config.sda_pin) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(config.sda_pin) ||
        !GPIO_IS_VALID_GPIO(config.scl_pin) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(config.scl_pin) ||
        config.sda_pin == config.scl_pin) {
        throw std::invalid_argument(
            "ESP I2C needs distinct bidirectional-capable SDA and SCL GPIOs");
    }
    if (config.glitch_ignore_count > kMaximumGlitchIgnoreCount ||
        config.interrupt_priority < 0) {
        throw std::invalid_argument("ESP I2C glitch filter or interrupt priority is invalid");
    }
#if !SOC_I2C_SUPPORT_SLEEP_RETENTION
    if (config.allow_power_down) {
        throw std::invalid_argument("ESP I2C target does not support peripheral power down");
    }
#endif
    if (config.minimum_frequency_hz < kMinimumFrequencyHz ||
        config.maximum_frequency_hz > kMaximumFrequencyHz ||
        config.minimum_frequency_hz > config.maximum_frequency_hz) {
        throw std::invalid_argument(
            "ESP I2C nominal clock range must be within 1000..400000 Hz");
    }
    if (config.maximum_transfer_bytes == 0 ||
        config.maximum_transfer_bytes > kMaximumTransferBytes ||
        config.maximum_pending_operations == 0 || config.io_timeout_ms == 0 ||
        config.io_timeout_ms > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        config.worker_stack_bytes < 3072) {
        throw std::invalid_argument("ESP I2C owner limits or worker settings are invalid");
    }
    if (config.allowed_addresses.empty()) {
        throw std::invalid_argument("ESP I2C endpoint needs an owner-allowed address range");
    }
    for (const auto& range : config.allowed_addresses) validate_address_range(range);
}

struct EspIdfI2cBackend::State final {
    enum class WorkState : std::uint8_t { free, queued, running };
    struct Work {
        BackendOperation operation{};
        std::weak_ptr<PeripheralCompletionSink> completions;
        WorkState state = WorkState::free;
        std::atomic<bool> cancelled{false};
        std::atomic<bool> retired{false};
        std::uint8_t* tx = nullptr;
        std::uint8_t* rx = nullptr;

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

    explicit State(const EspI2cEndpointConfig& configuration)
        : config(configuration), name(configuration.logical_name) {
        validate_esp_idf_i2c_config(config);
        try {
            allocate_work();
            i2c_master_bus_config_t bus{};
            bus.i2c_port = config.port;
            bus.sda_io_num = config.sda_pin;
            bus.scl_io_num = config.scl_pin;
            bus.clk_source = config.clock_source;
            bus.glitch_ignore_cnt = config.glitch_ignore_count;
            bus.intr_priority = config.interrupt_priority;
            // Synchronous calls retain all buffers until completion. Enabling
            // this queue would make IDF transactions asynchronous.
            bus.trans_queue_depth = 0;
            bus.flags.enable_internal_pullup = config.enable_internal_pullup;
            bus.flags.allow_pd = config.allow_power_down;
            check_construction(
                i2c_new_master_bus(&bus, &bus_handle), "initializing ESP I2C master bus");
            done = xSemaphoreCreateBinary();
            if (done == nullptr) {
                throw std::runtime_error("creating ESP I2C completion semaphore failed");
            }
            const auto task = xTaskCreate(
                &State::worker_entry, "mb_i2c", config.worker_stack_bytes, this,
                static_cast<UBaseType_t>(config.worker_priority), &worker);
            if (task != pdPASS) throw std::runtime_error("creating ESP I2C worker failed");
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
            work->tx = static_cast<std::uint8_t*>(heap_caps_malloc(
                config.maximum_transfer_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            work->rx = static_cast<std::uint8_t*>(heap_caps_malloc(
                config.maximum_transfer_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            if (work->tx == nullptr || work->rx == nullptr) {
                throw std::runtime_error("allocating ESP I2C internal staging buffers failed");
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
        if (bus_handle != nullptr) {
            (void)i2c_del_master_bus(bus_handle);
            bus_handle = nullptr;
        }
    }

    [[nodiscard]] IoStatus validate_address(
        std::uint16_t address, bool ten_bit) const noexcept {
        if (!valid_address_width(address, ten_bit)) return IoStatus::invalid_argument;
#if !SOC_I2C_SUPPORT_10BIT_ADDR
        if (ten_bit) return IoStatus::unsupported;
#endif
        for (const auto& range : config.allowed_addresses) {
            if (range.ten_bit == ten_bit && address >= range.first_address &&
                address <= range.last_address) {
                return IoStatus::completed;
            }
        }
        return IoStatus::permission_denied;
    }

    [[nodiscard]] IoStatus validate_plan(
        const TransactionPlan& plan, const DeviceBudget& budget) const noexcept {
        const IoStatus address_status = validate_address(plan.address, plan.ten_bit_address);
        if (address_status != IoStatus::completed) return address_status;
        if (plan.write_bytes.empty() && plan.read_bytes == 0) {
            return IoStatus::invalid_argument;
        }
        const auto maximum = std::min<std::uint64_t>(
            config.maximum_transfer_bytes, budget.maximum_bytes_per_buffer);
        if (plan.write_bytes.size() > maximum || plan.read_bytes > maximum) {
            return IoStatus::memory_exhausted;
        }
        return IoStatus::completed;
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
            if (minimum.capability != Capability::i2c_controller ||
                minimum.major != peripheral_abi_major) {
                return IoStatus::invalid_argument;
            }
            return minimum.minor == 0 ? IoStatus::completed : IoStatus::unsupported;
        }
        if (operation.device.is_empty() ||
            operation.device.capability != Capability::i2c_controller ||
            operation.device.owner_generation != operation.owner_generation) {
            return IoStatus::invalid_argument;
        }
        switch (operation.id) {
        case DeviceOperation::configure: {
            const auto* request = std::get_if<I2cConfigureRequest>(&operation.request);
            return request != nullptr && request->frequency_hz != 0
                       ? IoStatus::completed
                       : IoStatus::invalid_argument;
        }
        case DeviceOperation::read: {
            const auto* request = std::get_if<I2cReadRequest>(&operation.request);
            if (request == nullptr || request->byte_count == 0) {
                return IoStatus::invalid_argument;
            }
            return validate_plan(read_plan(*request), operation.budget);
        }
        case DeviceOperation::write: {
            const auto* request = std::get_if<I2cWriteRequest>(&operation.request);
            if (request == nullptr || request->bytes == nullptr || request->bytes->empty()) {
                return IoStatus::invalid_argument;
            }
            return validate_plan(write_plan(*request), operation.budget);
        }
        case DeviceOperation::transfer: {
            const auto* request = std::get_if<I2cTransferRequest>(&operation.request);
            if (request == nullptr || request->write_bytes == nullptr ||
                request->write_bytes->empty()) {
                return IoStatus::invalid_argument;
            }
            return validate_plan(transfer_plan(*request), operation.budget);
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
                return lease.generation == operation.owner_generation &&
                               (lease.closing || lease.release_pending)
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
            // Revoke the lease before physical cleanup. A rejected close never
            // reaches this state transition and therefore remains usable.
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
                OpenResponse{DeviceHandle{}, esp_idf_i2c_controller_version, false}};
    }

    [[nodiscard]] esp_err_t remove_device() noexcept {
        if (device_handle == nullptr) return ESP_OK;
        const esp_err_t result = i2c_master_bus_rm_device(device_handle);
        if (result == ESP_OK) device_handle = nullptr;
        return result;
    }

    void mark_unavailable() noexcept {
        std::lock_guard lock(mutex);
        unavailable = true;
    }

    [[nodiscard]] OperationResult close_physical() noexcept {
        const esp_err_t error = remove_device();
        configured = false;
        started = false;
        if (error != ESP_OK) mark_unavailable();
        return {i2c_status(error), EmptyResponse{}};
    }

    [[nodiscard]] OperationResult configure_endpoint(const BackendOperation& operation) {
        const IoStatus lease_status = bind_lease(operation);
        if (lease_status != IoStatus::completed) return {lease_status, EmptyResponse{}};
        if (started) return {IoStatus::busy, EmptyResponse{}};
        if (device_handle != nullptr) {
            mark_unavailable();
            return {IoStatus::device_lost, EmptyResponse{}};
        }
        const auto& request = std::get<I2cConfigureRequest>(operation.request);
        configuration.frequency_hz = std::clamp(
            request.frequency_hz, config.minimum_frequency_hz, config.maximum_frequency_hz);
        configured = true;
        // IDF sets its divider when a device transaction starts. No public
        // actual-clock query exists; report the exact nominal value installed
        // in each subsequently created i2c_device_config_t.
        return {IoStatus::completed, I2cConfigureResponse{configuration.frequency_hz}};
    }

    [[nodiscard]] esp_err_t execute_defined_stop_start(
        Work& work, const TransactionPlan& plan) noexcept {
        std::array<i2c_operation_job_t, 7> jobs{};
        std::size_t count = 0;
        jobs[count++].command = I2C_MASTER_CMD_START;
        jobs[count].command = I2C_MASTER_CMD_WRITE;
        jobs[count].write.ack_check = true;
        jobs[count].write.data = work.tx;
        jobs[count].write.total_bytes = plan.write_bytes.size();
        ++count;
        jobs[count++].command = I2C_MASTER_CMD_STOP;
        jobs[count++].command = I2C_MASTER_CMD_START;
        if (plan.read_bytes > 1) {
            jobs[count].command = I2C_MASTER_CMD_READ;
            jobs[count].read.ack_value = I2C_ACK_VAL;
            jobs[count].read.data = work.rx;
            jobs[count].read.total_bytes = plan.read_bytes - 1;
            ++count;
        }
        jobs[count].command = I2C_MASTER_CMD_READ;
        jobs[count].read.ack_value = I2C_NACK_VAL;
        jobs[count].read.data = work.rx + plan.read_bytes - 1;
        jobs[count].read.total_bytes = 1;
        ++count;
        jobs[count++].command = I2C_MASTER_CMD_STOP;
        return i2c_master_execute_defined_operations(
            device_handle, jobs.data(), count, static_cast<int>(config.io_timeout_ms));
    }


    [[nodiscard]] esp_err_t execute_transaction(
        Work& work, const TransactionPlan& plan) noexcept {
        if (plan.write_bytes.empty()) {
            return i2c_master_receive(
                device_handle, work.rx, plan.read_bytes,
                static_cast<int>(config.io_timeout_ms));
        }
        if (plan.read_bytes == 0) {
            return i2c_master_transmit(
                device_handle, work.tx, plan.write_bytes.size(),
                static_cast<int>(config.io_timeout_ms));
        }
        if (plan.repeated_start) {
            return i2c_master_transmit_receive(
                device_handle, work.tx, plan.write_bytes.size(), work.rx, plan.read_bytes,
                static_cast<int>(config.io_timeout_ms));
        }
        return execute_defined_stop_start(work, plan);
    }

    [[nodiscard]] static bool response_has_repeated_start(
        const TransactionPlan& plan) noexcept {
        return !plan.write_bytes.empty() && plan.read_bytes != 0 && plan.repeated_start;
    }

    [[nodiscard]] static OperationResult transaction_failure(
        const TransactionPlan&, esp_err_t error) noexcept {
        return {i2c_status(error),
                I2cTransferResponse{nullptr, 0, i2c_fault(error), false}};
    }

    [[nodiscard]] OperationResult transaction_success(
        Work& work, const TransactionPlan& plan) {
        ByteBufferPtr received;
        if (plan.read_bytes != 0) {
            received = make_immutable_bytes(
                std::span<const std::uint8_t>(work.rx, plan.read_bytes));
        }
        return {IoStatus::completed,
                I2cTransferResponse{std::move(received), plan.write_bytes.size(),
                                    I2cFault::none, response_has_repeated_start(plan)}};
    }

    [[nodiscard]] OperationResult transact(Work& work, const TransactionPlan& plan) {
        const IoStatus lease_status = bind_lease(work.operation);
        if (lease_status != IoStatus::completed) return {lease_status, EmptyResponse{}};
        if (unavailable) return {IoStatus::device_lost, EmptyResponse{}};
        if (!configured || !started) return {IoStatus::busy, EmptyResponse{}};
        if (device_handle != nullptr) {
            mark_unavailable();
            return {IoStatus::device_lost, EmptyResponse{}};
        }
        if (!plan.write_bytes.empty()) {
            std::memcpy(work.tx, plan.write_bytes.data(), plan.write_bytes.size());
        }

        i2c_device_config_t device{};
        device.dev_addr_length = plan.ten_bit_address
#if SOC_I2C_SUPPORT_10BIT_ADDR
                                     ? I2C_ADDR_BIT_LEN_10
#else
                                     ? I2C_ADDR_BIT_LEN_7
#endif
                                     : I2C_ADDR_BIT_LEN_7;
        device.device_address = plan.address;
        device.scl_speed_hz = configuration.frequency_hz;
        device.scl_wait_us = config.scl_wait_us;
        device.flags.disable_ack_check = false;
        esp_err_t error = i2c_master_bus_add_device(
            bus_handle, &device, &device_handle);
        if (error != ESP_OK) return transaction_failure(plan, error);

        error = execute_transaction(work, plan);
        const esp_err_t removal_error = remove_device();
        if (removal_error != ESP_OK) {
            mark_unavailable();
            return transaction_failure(plan, removal_error);
        }
        if (error != ESP_OK) return transaction_failure(plan, error);
        return transaction_success(work, plan);
    }

    [[nodiscard]] OperationResult perform(Work& work) {
        const auto& operation = work.operation;
        if (operation.id == DeviceOperation::close) return close_physical();
        if (cancellation_requested(work)) return {IoStatus::cancelled, EmptyResponse{}};
        switch (operation.id) {
        case DeviceOperation::open: return open_endpoint(operation);
        case DeviceOperation::configure: return configure_endpoint(operation);
        case DeviceOperation::read:
            return transact(work, read_plan(std::get<I2cReadRequest>(operation.request)));
        case DeviceOperation::write:
            return transact(work, write_plan(std::get<I2cWriteRequest>(operation.request)));
        case DeviceOperation::transfer:
            return transact(work,
                            transfer_plan(std::get<I2cTransferRequest>(operation.request)));
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
                ready.wait(lock, [&] {
                    return stopping || queue_size != 0 || lease.release_pending;
                });
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
        // Every active IDF call uses io_timeout_ms, and retired queued work does
        // not enter the hardware driver. A failed join must retain every buffer
        // and bus handle that a late IDF call or worker could still be using.
        if (worker_started &&
            xSemaphoreTake(done, detail::worker_shutdown_ticks(config.io_timeout_ms)) != pdTRUE) {
            ESP_LOGE(kLogTag, "I2C worker join timed out; retaining native resources");
            return false;
        }
        worker_started = false;
        (void)remove_device();
        if (bus_handle != nullptr) {
            const esp_err_t error = i2c_del_master_bus(bus_handle);
            if (error != ESP_OK) {
                ESP_LOGE(kLogTag, "deleting ESP I2C master bus: %s", esp_err_to_name(error));
            }
            bus_handle = nullptr;
        }
        if (done != nullptr) {
            vSemaphoreDelete(done);
            done = nullptr;
        }
        return true;
    }

    const EspI2cEndpointConfig config;
    std::string name;
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<std::unique_ptr<Work>> records;
    std::vector<Work*> queue;
    std::size_t queue_head = 0;
    std::size_t queue_size = 0;
    std::size_t pending_count = 0;
    Lease lease;
    i2c_master_bus_handle_t bus_handle = nullptr;
    i2c_master_dev_handle_t device_handle = nullptr;
    I2cConfigureRequest configuration{};
    bool configured = false;
    bool started = false;
    bool unavailable = false;
    bool accepting = true;
    bool stopping = false;
    bool stopped = false;
    SemaphoreHandle_t done = nullptr;
    TaskHandle_t worker = nullptr;
    bool worker_started = false;
};

EspIdfI2cBackend::EspIdfI2cBackend(const EspI2cEndpointConfig& config)
    : m_state(std::make_unique<State>(config)) {}
EspIdfI2cBackend::~EspIdfI2cBackend() {
    if (!m_state) return;
    if (m_state->stop()) m_state.reset();
    else (void)m_state.release();
}

std::span<const DeviceOperation> EspIdfI2cBackend::operations() const noexcept {
    return kOperations;
}

IoStatus EspIdfI2cBackend::start_operation(
    const BackendOperation& operation,
    std::weak_ptr<PeripheralCompletionSink> completions) {
    return m_state->start(operation, std::move(completions));
}

IoStatus EspIdfI2cBackend::cancel_operation(const BackendOperation& operation) {
    return m_state->cancel(operation);
}

void EspIdfI2cBackend::release_application(std::uint64_t owner_generation) {
    m_state->release(owner_generation);
}

EspIdfI2cRegistry::EspIdfI2cRegistry(
    PeripheralRegistry& registry, std::span<const EspI2cEndpointConfig> endpoints)
    : m_registry(registry) {
    for (std::size_t index = 0; index < endpoints.size(); ++index) {
        validate_esp_idf_i2c_config(endpoints[index]);
        for (std::size_t previous = 0; previous < index; ++previous) {
            const auto& before = endpoints[previous];
            const auto& current = endpoints[index];
            if (before.logical_name == current.logical_name || before.port == current.port) {
                throw std::invalid_argument("duplicate ESP I2C name or controller reservation");
            }
            if (before.sda_pin == current.sda_pin || before.sda_pin == current.scl_pin ||
                before.scl_pin == current.sda_pin || before.scl_pin == current.scl_pin) {
                throw std::invalid_argument("duplicate ESP I2C physical GPIO reservation");
            }
        }
    }
    m_backends.reserve(endpoints.size());
    for (const auto& config : endpoints) {
        m_backends.push_back(std::make_shared<EspIdfI2cBackend>(config));
        m_registry.register_endpoint(
            {config.logical_name, esp_idf_i2c_controller_version, false},
            EndpointAccess::application, m_backends.back());
    }
}

} // namespace mb
