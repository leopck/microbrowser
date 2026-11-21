#include "esp_idf_peripherals.h"

#include "esp_idf_worker.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace mb {
namespace {

constexpr char kLogTag[] = "mb_gpio";
constexpr std::size_t kMaximumEndpoints = 8;
constexpr std::size_t kMaximumOperationSlots = 8;
constexpr std::size_t kInterruptRecordAllowance = 32;
constexpr std::size_t kWorkerQueueLength =
    kMaximumOperationSlots + kInterruptRecordAllowance + 2;
constexpr std::size_t kMaximumLogicalNameBytes = 63;
constexpr std::uint32_t kWorkerStackBytes = 4096;
constexpr UBaseType_t kWorkerPriority = 5;

constexpr std::array<DeviceOperation, 8> kImplementedOperations{
    DeviceOperation::open,
    DeviceOperation::configure,
    DeviceOperation::read,
    DeviceOperation::write,
    DeviceOperation::subscribe,
    DeviceOperation::unsubscribe,
    DeviceOperation::close,
    DeviceOperation::cancel,
};

[[nodiscard]] constexpr EspGpioOperations all_gpio_operations() noexcept {
    return EspGpioOperations::configure | EspGpioOperations::read |
           EspGpioOperations::write | EspGpioOperations::subscribe;
}

[[nodiscard]] bool operation_request_matches(const BackendOperation& operation) noexcept {
    switch (operation.id) {
    case DeviceOperation::open:
        return std::holds_alternative<OpenRequest>(operation.request);
    case DeviceOperation::configure:
        return std::holds_alternative<GpioConfigureRequest>(operation.request);
    case DeviceOperation::read:
        return std::holds_alternative<GpioReadRequest>(operation.request);
    case DeviceOperation::write:
        return std::holds_alternative<GpioWriteRequest>(operation.request);
    case DeviceOperation::subscribe:
        return std::holds_alternative<GpioSubscribeRequest>(operation.request);
    case DeviceOperation::unsubscribe:
        return std::holds_alternative<UnsubscribeRequest>(operation.request);
    case DeviceOperation::close:
        return std::holds_alternative<CloseRequest>(operation.request);
    default:
        return false;
    }
}

[[nodiscard]] EspGpioOperations required_owner_operation(
    DeviceOperation operation) noexcept {
    switch (operation) {
    case DeviceOperation::configure: return EspGpioOperations::configure;
    case DeviceOperation::read: return EspGpioOperations::read;
    case DeviceOperation::write: return EspGpioOperations::write;
    case DeviceOperation::subscribe: return EspGpioOperations::subscribe;
    default: return EspGpioOperations::none;
    }
}

[[nodiscard]] IoStatus gpio_error_status(esp_err_t error) noexcept {
    switch (error) {
    case ESP_OK: return IoStatus::completed;
    case ESP_ERR_INVALID_ARG: return IoStatus::invalid_argument;
    case ESP_ERR_NO_MEM: return IoStatus::memory_exhausted;
    case ESP_ERR_NOT_SUPPORTED: return IoStatus::unsupported;
    case ESP_ERR_TIMEOUT: return IoStatus::timeout;
    default: return IoStatus::io_failure;
    }
}

[[noreturn]] void throw_gpio_error(std::string_view action, esp_err_t error) {
    std::string message(action);
    message += ": ";
    message += esp_err_to_name(error);
    throw std::runtime_error(message);
}

void check_construction_call(esp_err_t error, std::string_view action) {
    if (error != ESP_OK) throw_gpio_error(action, error);
}

void log_gpio_error(std::string_view action, gpio_num_t pin, esp_err_t error) noexcept {
    ESP_LOGE(kLogTag, "%.*s GPIO%d: %s", static_cast<int>(action.size()),
             action.data(), static_cast<int>(pin), esp_err_to_name(error));
}

[[nodiscard]] std::int64_t monotonic_time_ns() noexcept {
    const std::int64_t microseconds = esp_timer_get_time();
    constexpr std::int64_t scale = 1000;
    if (microseconds > std::numeric_limits<std::int64_t>::max() / scale) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (microseconds < std::numeric_limits<std::int64_t>::min() / scale) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return microseconds * scale;
}

[[nodiscard]] std::optional<gpio_int_type_t> interrupt_type(GpioEdge edge) noexcept {
    switch (edge) {
    case GpioEdge::rising: return GPIO_INTR_POSEDGE;
    case GpioEdge::falling: return GPIO_INTR_NEGEDGE;
    case GpioEdge::both: return GPIO_INTR_ANYEDGE;
    }
    return std::nullopt;
}

[[nodiscard]] bool valid_pull(GpioPull pull) noexcept {
    return pull == GpioPull::none || pull == GpioPull::up || pull == GpioPull::down;
}

struct OperationResult {
    IoStatus status = IoStatus::invalid_argument;
    DeviceResponse response = EmptyResponse{};
    bool completion_sent = false;
};

} // namespace

struct EspIdfGpioBackend::State final {
    struct EndpointState {
        EspGpioEndpointConfig config{};
        std::array<char, kMaximumLogicalNameBytes> logical_name{};
        std::size_t logical_name_size = 0;
        QueueHandle_t worker_queue = nullptr;

        State* owner = nullptr;
        std::uint32_t interrupt_serial = 0;

        // The lease fields are accessed by tasks under State::mutex. The ISR
        // Lease fields are accessed by tasks under State::mutex. The counter is
        // read and written under the same mutex, including the short ISR access.
        bool open = false;
        bool configured = false;
        bool release_requested = false;
        bool handler_added = false;
        std::uint64_t owner_generation = 0;
        DeviceHandle device{};

        bool subscription_active = false;
        std::uint32_t subscription_serial = 0;
        std::uint64_t subscription_owner = 0;
        DeviceHandle subscription{};
        std::weak_ptr<PeripheralCompletionSink> subscription_completions;
    };

    enum class WorkState : std::uint8_t { free, queued, running };
    struct WorkRecord {
        BackendOperation operation{};
        std::weak_ptr<PeripheralCompletionSink> completions;
        EndpointState* endpoint = nullptr;
        WorkState state = WorkState::free;
        std::atomic<bool> cancellation_requested{false};
        std::atomic<bool> retirement_requested{false};
    };

    enum class WorkerMessageType : std::uint8_t {
        operation,
        interrupt,
        wake,
        stop,
    };
    struct WorkerMessage {
        WorkerMessageType type = WorkerMessageType::wake;
        WorkRecord* work = nullptr;
        EndpointState* endpoint = nullptr;
        std::uint32_t serial = 0;
        std::int64_t timestamp_ns = 0;
        bool high = false;
    };
    static_assert(std::is_trivially_copyable_v<WorkerMessage>);
    // endpoint.interrupt_serial is synchronized rather than relying on an
    // architecture-specific 32-bit atomic implementation in interrupt context.

    explicit State(std::span<const EspGpioEndpointConfig> configs) {
        validate_configs(configs);
        m_endpoint_count = configs.size();
        for (std::size_t index = 0; index < configs.size(); ++index) {
            copy_config(m_endpoints[index], configs[index]);
        }
        try {
            create_native_objects();
            install_interrupt_routes();
            start_worker();
        } catch (...) {
            cleanup_failed_construction();
            throw;
        }
    }

    ~State() = default;

    static void validate_configs(std::span<const EspGpioEndpointConfig> configs) {
        if (configs.size() > kMaximumEndpoints) {
            throw std::invalid_argument("ESP GPIO supports at most eight endpoint aliases");
        }
        const auto allowed_mask = static_cast<std::uint8_t>(all_gpio_operations());
        for (std::size_t index = 0; index < configs.size(); ++index) {
            const auto& config = configs[index];
            if (config.logical_name.empty() ||
                config.logical_name.size() > kMaximumLogicalNameBytes) {
                throw std::invalid_argument("ESP GPIO logical names must contain 1..63 bytes");
            }
            if (!GPIO_IS_VALID_GPIO(config.pin) ||
                (config.direction == GpioDirection::output &&
                 !GPIO_IS_VALID_OUTPUT_GPIO(config.pin))) {
                throw std::invalid_argument("ESP GPIO owner configuration names an invalid pin");
            }
            if (config.operations == EspGpioOperations::none) {
                throw std::invalid_argument("ESP GPIO endpoint needs at least one owner-allowed operation");
            }
            if (config.direction != GpioDirection::input &&
                config.direction != GpioDirection::output) {
                throw std::invalid_argument("ESP GPIO owner configuration names an invalid direction");
            }
            if ((static_cast<std::uint8_t>(config.operations) &
                 static_cast<std::uint8_t>(~allowed_mask)) != 0) {
                throw std::invalid_argument("ESP GPIO owner configuration has unknown operations");
            }
            for (std::size_t previous = 0; previous < index; ++previous) {
                if (configs[previous].pin == config.pin) {
                    throw std::invalid_argument("duplicate ESP GPIO physical pin assignment");
                }
                if (configs[previous].logical_name == config.logical_name) {
                    throw std::invalid_argument("duplicate ESP GPIO logical endpoint name");
                }
            }
        }
    }

    void copy_config(EndpointState& endpoint,
                            const EspGpioEndpointConfig& config) noexcept {
        endpoint.owner = this;
        endpoint.config = config;
        endpoint.logical_name_size = config.logical_name.size();
        std::copy(config.logical_name.begin(), config.logical_name.end(),
                  endpoint.logical_name.begin());
    }

    void set_interrupt_serial(EndpointState& endpoint, std::uint32_t serial) noexcept {
        portENTER_CRITICAL(&m_mutex);
        endpoint.interrupt_serial = serial;
        portEXIT_CRITICAL(&m_mutex);
    }

    [[nodiscard]] std::uint32_t interrupt_serial_for_isr(const EndpointState& endpoint) noexcept {
        portENTER_CRITICAL_ISR(&m_mutex);
        const std::uint32_t serial = endpoint.interrupt_serial;
        portEXIT_CRITICAL_ISR(&m_mutex);
        return serial;
    }

    [[nodiscard]] std::string_view logical_name(const EndpointState& endpoint) const noexcept {
        return {endpoint.logical_name.data(), endpoint.logical_name_size};
    }

    [[nodiscard]] EndpointState* find_endpoint(std::string_view name) noexcept {
        for (std::size_t index = 0; index < m_endpoint_count; ++index) {
            EndpointState& endpoint = m_endpoints[index];
            if (logical_name(endpoint) == name) return &endpoint;
        }
        return nullptr;
    }

    void create_native_objects() {
        m_worker_queue = xQueueCreateStatic(
            kWorkerQueueLength, sizeof(WorkerMessage), m_queue_storage.data(),
            &m_queue_control);
        m_worker_done = xSemaphoreCreateBinaryStatic(&m_worker_done_control);
        if (m_worker_queue == nullptr || m_worker_done == nullptr) {
            throw std::runtime_error("creating static ESP GPIO FreeRTOS objects failed");
        }
        for (std::size_t index = 0; index < m_endpoint_count; ++index) {
            m_endpoints[index].worker_queue = m_worker_queue;
        }
    }

    void install_interrupt_routes() {
        const esp_err_t install_status = gpio_install_isr_service(0);
        if (install_status == ESP_OK) {
            m_owns_isr_service = true;
        } else if (install_status != ESP_ERR_INVALID_STATE) {
            throw_gpio_error("installing ESP GPIO ISR service", install_status);
        }

        for (std::size_t index = 0; index < m_endpoint_count; ++index) {
            EndpointState& endpoint = m_endpoints[index];
            check_construction_call(
                gpio_isr_handler_add(endpoint.config.pin, &State::gpio_isr, &endpoint),
                "adding ESP GPIO pin ISR");
            endpoint.handler_added = true;
            check_construction_call(gpio_intr_disable(endpoint.config.pin),
                                    "disabling initial ESP GPIO IRQ");
        }
    }

    void start_worker() {
        m_worker = xTaskCreateStatic(
            &State::worker_entry, "mb_gpio", kWorkerStackBytes, this, kWorkerPriority,
            reinterpret_cast<StackType_t*>(m_worker_stack.data()), &m_worker_control);
        if (m_worker == nullptr) {
            throw std::runtime_error("creating static ESP GPIO worker task failed");
        }
        m_worker_started = true;
    }

    void cleanup_failed_construction() noexcept {
        remove_interrupt_routes();
        if (m_owns_isr_service) {
            (void)gpio_uninstall_isr_service();
            m_owns_isr_service = false;
        }
        delete_native_objects();
    }

    void delete_native_objects() noexcept {
        if (m_worker_queue != nullptr) {
            vQueueDelete(m_worker_queue);
            m_worker_queue = nullptr;
        }
        if (m_worker_done != nullptr) {
            vSemaphoreDelete(m_worker_done);
            m_worker_done = nullptr;
        }
    }

    void remove_interrupt_routes() noexcept {
        for (std::size_t index = 0; index < m_endpoint_count; ++index) {
            EndpointState& endpoint = m_endpoints[index];
            if (!endpoint.handler_added) continue;
            set_interrupt_serial(endpoint, 0);
            const esp_err_t disable_status = gpio_intr_disable(endpoint.config.pin);
            if (disable_status != ESP_OK) {
                log_gpio_error("disabling ESP GPIO IRQ", endpoint.config.pin, disable_status);
            }
            const esp_err_t remove_status = gpio_isr_handler_remove(endpoint.config.pin);
            if (remove_status != ESP_OK) {
                log_gpio_error("removing ESP GPIO pin ISR", endpoint.config.pin, remove_status);
            }
            endpoint.handler_added = false;
        }
    }

    [[nodiscard]] IoStatus start(
        const BackendOperation& operation,
        std::weak_ptr<PeripheralCompletionSink> completions) noexcept {
        EndpointState* const endpoint = find_endpoint(operation.logical_name);
        if (endpoint == nullptr) return IoStatus::invalid_argument;
        if (!operation_request_matches(operation)) return IoStatus::invalid_argument;
        if (operation.id == DeviceOperation::open) {
            const auto& request = std::get<OpenRequest>(operation.request).minimum_version;
            if (request.capability != Capability::gpio) return IoStatus::invalid_argument;
            if (request.major != peripheral_abi_major || request.minor != 0) {
                return IoStatus::unsupported;
            }
        }
        if (operation.id == DeviceOperation::configure) {
            const auto& request = std::get<GpioConfigureRequest>(operation.request);
            if (!valid_pull(request.pull) ||
                (request.direction != GpioDirection::input &&
                 request.direction != GpioDirection::output)) {
                return IoStatus::invalid_argument;
            }
        }
        if ((operation.id == DeviceOperation::subscribe &&
             (!interrupt_type(std::get<GpioSubscribeRequest>(operation.request).edge) ||
              operation.subscription.is_empty())) ||
            (operation.id == DeviceOperation::unsubscribe &&
             std::get<UnsubscribeRequest>(operation.request).subscription.is_empty())) {
            return IoStatus::invalid_argument;
        }

        const EspGpioOperations required = required_owner_operation(operation.id);
        if (required != EspGpioOperations::none &&
            !esp_gpio_operation_allowed(endpoint->config.operations, required)) {
            return IoStatus::permission_denied;
        }
        if (endpoint->config.direction == GpioDirection::input &&
            operation.id == DeviceOperation::write) {
            return IoStatus::permission_denied;
        }
        if (operation.id == DeviceOperation::configure) {
            const auto& configure = std::get<GpioConfigureRequest>(operation.request);
            if (configure.direction != endpoint->config.direction) {
                return IoStatus::permission_denied;
            }
        }

        WorkRecord* selected = nullptr;
        bool accepting = false;
        portENTER_CRITICAL(&m_mutex);
        accepting = m_accepting;
        if (accepting) {
            for (WorkRecord& work : m_work) {
                if (work.state != WorkState::free) continue;
                selected = &work;
                work.operation = operation;
                work.completions = std::move(completions);
                work.endpoint = endpoint;
                work.cancellation_requested.store(false, std::memory_order_release);
                work.retirement_requested.store(false, std::memory_order_release);
                work.state = WorkState::queued;
                break;
            }
        }
        portEXIT_CRITICAL(&m_mutex);
        if (selected == nullptr) return accepting ? IoStatus::busy : IoStatus::device_lost;

        WorkerMessage message{};
        message.type = WorkerMessageType::operation;
        message.work = selected;
        if (xQueueSend(m_worker_queue, &message, 0) != pdTRUE) {
            portENTER_CRITICAL(&m_mutex);
            selected->state = WorkState::free;
            selected->endpoint = nullptr;
            selected->completions.reset();
            selected->operation = BackendOperation{};
            portEXIT_CRITICAL(&m_mutex);
            return IoStatus::busy;
        }
        return IoStatus::completed;
    }

    [[nodiscard]] IoStatus cancel(const BackendOperation& operation) noexcept {
        portENTER_CRITICAL(&m_mutex);
        for (WorkRecord& work : m_work) {
            if (work.state == WorkState::free) continue;
            if (work.operation.owner_generation != operation.owner_generation ||
                work.operation.operation != operation.operation) {
                continue;
            }
            work.cancellation_requested.store(true, std::memory_order_release);
            portEXIT_CRITICAL(&m_mutex);
            return IoStatus::completed;
        }
        portEXIT_CRITICAL(&m_mutex);
        return IoStatus::invalid_argument;
    }

    void release(std::uint64_t owner_generation) noexcept {
        portENTER_CRITICAL(&m_mutex);
        for (WorkRecord& work : m_work) {
            if (work.state != WorkState::free &&
                work.operation.owner_generation == owner_generation) {
                work.retirement_requested.store(true, std::memory_order_release);
            }
        }
        for (std::size_t index = 0; index < m_endpoint_count; ++index) {
            EndpointState& endpoint = m_endpoints[index];
            if (endpoint.open && endpoint.owner_generation == owner_generation) {
                endpoint.release_requested = true;
            }
        }
        portEXIT_CRITICAL(&m_mutex);

        WorkerMessage message{};
        message.type = WorkerMessageType::wake;
        // A full queue already guarantees worker progress. No heap allocation or
        // worker rendezvous is needed to release an application.
        (void)xQueueSend(m_worker_queue, &message, 0);
    }

    [[nodiscard]] static bool cancelled(const WorkRecord& work) noexcept {
        return work.cancellation_requested.load(std::memory_order_acquire) ||
               work.retirement_requested.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool complete(WorkRecord& work, IoStatus status,
                                DeviceResponse response) {
        const auto completions = work.completions.lock();
        if (!completions) return false;
        completions->complete(work.operation, status, std::move(response));
        return true;
    }

    [[nodiscard]] IoStatus bind_device_lease(
        EndpointState& endpoint, const BackendOperation& operation) noexcept {
        portENTER_CRITICAL(&m_mutex);
        IoStatus status = IoStatus::completed;
        if (!endpoint.open || endpoint.owner_generation != operation.owner_generation) {
            status = IoStatus::device_lost;
        } else if (endpoint.device.is_empty()) {
            // DeviceSession creates this handle after open completion. The first
            // device operation binds it so a later owner cannot use this lease.
            endpoint.device = operation.device;
        } else if (endpoint.device != operation.device) {
            status = IoStatus::invalid_argument;
        }
        portEXIT_CRITICAL(&m_mutex);
        return status;
    }

    [[nodiscard]] OperationResult open_endpoint(
        EndpointState& endpoint, const BackendOperation& operation) {
        portENTER_CRITICAL(&m_mutex);
        const bool busy = endpoint.open;
        portEXIT_CRITICAL(&m_mutex);
        if (busy) return {IoStatus::busy, EmptyResponse{}};

        const esp_err_t disable_status = gpio_intr_disable(endpoint.config.pin);
        if (disable_status != ESP_OK) return {gpio_error_status(disable_status), EmptyResponse{}};

        portENTER_CRITICAL(&m_mutex);
        endpoint.open = true;
        endpoint.configured = false;
        endpoint.release_requested = false;
        endpoint.owner_generation = operation.owner_generation;
        endpoint.device = DeviceHandle{};
        portEXIT_CRITICAL(&m_mutex);
        return {IoStatus::completed, OpenResponse{DeviceHandle{}, esp_idf_gpio_version, false}};
    }

    [[nodiscard]] gpio_pullup_t pull_up(GpioPull pull) const noexcept {
        return pull == GpioPull::up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    }

    [[nodiscard]] gpio_pulldown_t pull_down(GpioPull pull) const noexcept {
        return pull == GpioPull::down ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    }

    [[nodiscard]] OperationResult configure_endpoint(
        EndpointState& endpoint, const BackendOperation& operation) {
        const IoStatus lease_status = bind_device_lease(endpoint, operation);
        if (lease_status != IoStatus::completed) return {lease_status, EmptyResponse{}};
        const auto& request = std::get<GpioConfigureRequest>(operation.request);
        if (request.direction != endpoint.config.direction) {
            return {IoStatus::permission_denied, EmptyResponse{}};
        }

        portENTER_CRITICAL(&m_mutex);
        const bool subscription_active = endpoint.subscription_active;
        portEXIT_CRITICAL(&m_mutex);
        if (subscription_active) return {IoStatus::busy, EmptyResponse{}};

        if (request.direction == GpioDirection::output) {
            // Set the latch before enabling output, avoiding an initial-low glitch
            // when the owner requests initial_high.
            const esp_err_t level_status = gpio_set_level(endpoint.config.pin, request.initial_high);
            if (level_status != ESP_OK) {
                return {gpio_error_status(level_status), EmptyResponse{}};
            }
        }
        const bool readable_output = request.direction == GpioDirection::output &&
            (esp_gpio_operation_allowed(endpoint.config.operations, EspGpioOperations::read) ||
             esp_gpio_operation_allowed(endpoint.config.operations, EspGpioOperations::subscribe));
        const gpio_mode_t mode = request.direction == GpioDirection::input
            ? GPIO_MODE_INPUT
            : (readable_output ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_OUTPUT);
        const gpio_config_t config{
            .pin_bit_mask = 1ULL << static_cast<std::uint32_t>(endpoint.config.pin),
            .mode = mode,
            .pull_up_en = pull_up(request.pull),
            .pull_down_en = pull_down(request.pull),
            .intr_type = GPIO_INTR_DISABLE,
        };
        const esp_err_t config_status = gpio_config(&config);
        if (config_status != ESP_OK) return {gpio_error_status(config_status), EmptyResponse{}};

        portENTER_CRITICAL(&m_mutex);
        endpoint.configured = true;
        portEXIT_CRITICAL(&m_mutex);
        return {IoStatus::completed, EmptyResponse{}};
    }

    [[nodiscard]] OperationResult read_endpoint(
        EndpointState& endpoint, const BackendOperation& operation) {
        const IoStatus lease_status = bind_device_lease(endpoint, operation);
        if (lease_status != IoStatus::completed) return {lease_status, EmptyResponse{}};
        if (!endpoint.configured) return {IoStatus::invalid_argument, EmptyResponse{}};
        const bool high = gpio_get_level(endpoint.config.pin) != 0;
        return {IoStatus::completed, GpioReadResponse{high, monotonic_time_ns()}};
    }

    [[nodiscard]] OperationResult write_endpoint(
        EndpointState& endpoint, const BackendOperation& operation) {
        if (endpoint.config.direction != GpioDirection::output) {
            return {IoStatus::permission_denied, EmptyResponse{}};
        }
        const IoStatus lease_status = bind_device_lease(endpoint, operation);
        if (lease_status != IoStatus::completed) return {lease_status, EmptyResponse{}};
        if (!endpoint.configured) return {IoStatus::invalid_argument, EmptyResponse{}};
        const auto& request = std::get<GpioWriteRequest>(operation.request);
        const esp_err_t write_status = gpio_set_level(endpoint.config.pin, request.high ? 1U : 0U);
        return {gpio_error_status(write_status), EmptyResponse{}};
    }

    void clear_subscription(EndpointState& endpoint) noexcept {
        set_interrupt_serial(endpoint, 0);
        portENTER_CRITICAL(&m_mutex);
        endpoint.subscription_active = false;
        endpoint.subscription_serial = 0;
        endpoint.subscription_owner = 0;
        endpoint.subscription = DeviceHandle{};
        endpoint.subscription_completions.reset();
        portEXIT_CRITICAL(&m_mutex);
    }

    [[nodiscard]] OperationResult subscribe_endpoint(
        EndpointState& endpoint, WorkRecord& work) {
        const BackendOperation& operation = work.operation;
        const IoStatus lease_status = bind_device_lease(endpoint, operation);
        if (lease_status != IoStatus::completed) return {lease_status, EmptyResponse{}};
        if (!endpoint.configured) return {IoStatus::invalid_argument, EmptyResponse{}};

        portENTER_CRITICAL(&m_mutex);
        const bool subscription_active = endpoint.subscription_active;
        portEXIT_CRITICAL(&m_mutex);
        if (subscription_active) return {IoStatus::busy, EmptyResponse{}};

        const auto& request = std::get<GpioSubscribeRequest>(operation.request);
        const auto irq_type = interrupt_type(request.edge);
        if (!irq_type) return {IoStatus::invalid_argument, EmptyResponse{}};
        esp_err_t status = gpio_intr_disable(endpoint.config.pin);
        if (status == ESP_OK) {
            status = gpio_set_intr_type(endpoint.config.pin, *irq_type);
        }
        if (status != ESP_OK) return {gpio_error_status(status), EmptyResponse{}};

        // Enable the actual IRQ route while its serial is zero, so an in-flight
        // edge is discarded. Only after the ready completion is queued does the
        // nonzero serial make captured levels eligible for publication.
        status = gpio_intr_enable(endpoint.config.pin);
        if (status != ESP_OK) return {gpio_error_status(status), EmptyResponse{}};
        if (cancelled(work)) {
            const esp_err_t disable_status = gpio_intr_disable(endpoint.config.pin);
            if (disable_status != ESP_OK) {
                log_gpio_error("disabling cancelled ESP GPIO subscription",
                               endpoint.config.pin, disable_status);
            }
            return {IoStatus::cancelled, EmptyResponse{}};
        }

        std::uint32_t serial = m_next_interrupt_serial++;
        if (m_next_interrupt_serial == 0) m_next_interrupt_serial = 1;
        portENTER_CRITICAL(&m_mutex);
        endpoint.subscription_active = true;
        endpoint.subscription_serial = serial;
        endpoint.subscription_owner = operation.owner_generation;
        endpoint.subscription = operation.subscription;
        endpoint.subscription_completions = work.completions;
        portEXIT_CRITICAL(&m_mutex);

        const bool delivered = complete(
            work, IoStatus::completed, SubscriptionResponse{operation.subscription});
        if (!delivered) {
            const esp_err_t disable_status = gpio_intr_disable(endpoint.config.pin);
            if (disable_status != ESP_OK) {
                log_gpio_error("stopping unobserved ESP GPIO subscription",
                               endpoint.config.pin, disable_status);
            }
            clear_subscription(endpoint);
            return {IoStatus::device_lost, EmptyResponse{}};
        }
        set_interrupt_serial(endpoint, serial);
        return {IoStatus::completed, std::monostate{}, true};
    }

    [[nodiscard]] esp_err_t stop_subscription(EndpointState& endpoint) noexcept {
        const esp_err_t status = gpio_intr_disable(endpoint.config.pin);
        if (status != ESP_OK) return status;
        clear_subscription(endpoint);
        return ESP_OK;
    }

    [[nodiscard]] OperationResult unsubscribe_endpoint(
        EndpointState& endpoint, const BackendOperation& operation) {
        const IoStatus lease_status = bind_device_lease(endpoint, operation);
        if (lease_status != IoStatus::completed) return {lease_status, EmptyResponse{}};
        const auto& request = std::get<UnsubscribeRequest>(operation.request);
        portENTER_CRITICAL(&m_mutex);
        const bool matches = endpoint.subscription_active &&
            endpoint.subscription == request.subscription;
        portEXIT_CRITICAL(&m_mutex);
        if (!matches) return {IoStatus::invalid_argument, EmptyResponse{}};
        const esp_err_t stop_status = stop_subscription(endpoint);
        return {gpio_error_status(stop_status), EmptyResponse{}};
    }

    [[nodiscard]] esp_err_t close_physical_lease(EndpointState& endpoint) noexcept {
        const esp_err_t disable_status = gpio_intr_disable(endpoint.config.pin);
        if (disable_status != ESP_OK) return disable_status;
        clear_subscription(endpoint);
        portENTER_CRITICAL(&m_mutex);
        endpoint.open = false;
        endpoint.configured = false;
        endpoint.release_requested = false;
        endpoint.owner_generation = 0;
        endpoint.device = DeviceHandle{};
        portEXIT_CRITICAL(&m_mutex);
        return ESP_OK;
    }

    [[nodiscard]] OperationResult close_endpoint(
        EndpointState& endpoint, const BackendOperation& operation) {
        const IoStatus lease_status = bind_device_lease(endpoint, operation);
        if (lease_status != IoStatus::completed) return {lease_status, EmptyResponse{}};
        const esp_err_t close_status = close_physical_lease(endpoint);
        return {gpio_error_status(close_status), EmptyResponse{}};
    }

    [[nodiscard]] OperationResult perform_operation(WorkRecord& work) {
        EndpointState& endpoint = *work.endpoint;
        const BackendOperation& operation = work.operation;
        if (operation.id == DeviceOperation::close) {
            return close_endpoint(endpoint, operation);
        }
        if (operation.id == DeviceOperation::unsubscribe) {
            return unsubscribe_endpoint(endpoint, operation);
        }
        if (cancelled(work)) return {IoStatus::cancelled, EmptyResponse{}};

        switch (operation.id) {
        case DeviceOperation::open: return open_endpoint(endpoint, operation);
        case DeviceOperation::configure: return configure_endpoint(endpoint, operation);
        case DeviceOperation::read: return read_endpoint(endpoint, operation);
        case DeviceOperation::write: return write_endpoint(endpoint, operation);
        case DeviceOperation::subscribe: return subscribe_endpoint(endpoint, work);
        default: return {IoStatus::unsupported, EmptyResponse{}};
        }
    }

    void process_operation(WorkRecord& work) {
        portENTER_CRITICAL(&m_mutex);
        work.state = WorkState::running;
        portEXIT_CRITICAL(&m_mutex);

        OperationResult result = perform_operation(work);
                if (work.operation.id == DeviceOperation::open &&
                    result.status == IoStatus::completed && cancelled(work)) {
                    const esp_err_t status = close_physical_lease(*work.endpoint);
                    if (status != ESP_OK) {
                        log_gpio_error("closing a cancelled ESP GPIO open", work.endpoint->config.pin, status);
                    }
                    result = {IoStatus::cancelled, EmptyResponse{}};
                }
        if ((work.operation.id == DeviceOperation::close ||
             work.operation.id == DeviceOperation::unsubscribe) && cancelled(work)) {
            result = {IoStatus::cancelled, EmptyResponse{}};
        }
        if (!result.completion_sent) {
            // subscribe_endpoint sent its successful ready completion before
            // making the IRQ route live, so it must not be completed twice.
            (void)complete(work, result.status, std::move(result.response));
        }

        portENTER_CRITICAL(&m_mutex);
        work.state = WorkState::free;
        work.endpoint = nullptr;
        work.operation = BackendOperation{};
        work.completions.reset();
        portEXIT_CRITICAL(&m_mutex);
    }

    void process_releases() {
        for (std::size_t index = 0; index < m_endpoint_count; ++index) {
            EndpointState& endpoint = m_endpoints[index];
            portENTER_CRITICAL(&m_mutex);
            const bool release = endpoint.release_requested && endpoint.open;
            if (release) endpoint.release_requested = false;
            portEXIT_CRITICAL(&m_mutex);
            if (!release) continue;
            const esp_err_t status = close_physical_lease(endpoint);
            if (status != ESP_OK) {
                // Keeping the lease occupied prevents a later owner from
                // inheriting a pin whose IRQ could not be disabled.
                log_gpio_error("releasing ESP GPIO application lease",
                               endpoint.config.pin, status);
            }
        }
    }

    void publish_interrupt(const WorkerMessage& message) {
        EndpointState& endpoint = *message.endpoint;
        portENTER_CRITICAL(&m_mutex);
        const bool current = endpoint.subscription_active &&
            endpoint.interrupt_serial == message.serial &&
            endpoint.subscription_serial == message.serial;
        const std::uint64_t owner = endpoint.subscription_owner;
        const DeviceHandle subscription = endpoint.subscription;
        auto completions = endpoint.subscription_completions.lock();
        portEXIT_CRITICAL(&m_mutex);
        if (!current || !completions) return;

        // Dropping on busy/memory_exhausted/device_lost is deliberate bounded
        // backpressure. GPIO levels never allocate retry records here.
        (void)completions->publish(
            owner, subscription, GpioReadResponse{message.high, message.timestamp_ns});
    }

    void stop_all() noexcept {
        for (std::size_t index = 0; index < m_endpoint_count; ++index) {
            EndpointState& endpoint = m_endpoints[index];
            const esp_err_t status = gpio_intr_disable(endpoint.config.pin);
            if (status != ESP_OK) {
                log_gpio_error("stopping ESP GPIO worker IRQ", endpoint.config.pin, status);
            }
            clear_subscription(endpoint);
            portENTER_CRITICAL(&m_mutex);
            endpoint.open = false;
            endpoint.configured = false;
            endpoint.owner_generation = 0;
            endpoint.device = DeviceHandle{};
            portEXIT_CRITICAL(&m_mutex);
        }
        remove_interrupt_routes();
    }

    static void worker_entry(void* argument) {
        auto& state = *static_cast<State*>(argument);
        state.run_worker();
        xSemaphoreGive(state.m_worker_done);
        vTaskDelete(nullptr);
    }

    void run_worker() {
        for (;;) {
            WorkerMessage message{};
            if (xQueueReceive(m_worker_queue, &message, portMAX_DELAY) != pdTRUE) continue;
            process_releases();
            if (message.type == WorkerMessageType::stop) break;
            if (message.type == WorkerMessageType::operation && message.work != nullptr) {
                process_operation(*message.work);
            } else if (message.type == WorkerMessageType::interrupt &&
                       message.endpoint != nullptr) {
                publish_interrupt(message);
            }
        }
        stop_all();
    }

    static void gpio_isr(void* argument) {
        auto& endpoint = *static_cast<EndpointState*>(argument);
        const std::uint32_t serial = endpoint.owner->interrupt_serial_for_isr(endpoint);
        if (serial == 0) return;

        // This is the complete ISR payload. In particular, it has no weak_ptr,
        // DeviceResponse, lock, heap allocation, or application/VM reference.
        WorkerMessage message{};
        message.type = WorkerMessageType::interrupt;
        message.endpoint = &endpoint;
        message.serial = serial;
        message.timestamp_ns = monotonic_time_ns();
        message.high = gpio_get_level(endpoint.config.pin) != 0;
        BaseType_t higher_priority_woken = pdFALSE;
        if (xQueueSendFromISR(endpoint.worker_queue, &message, &higher_priority_woken) == pdTRUE &&
            higher_priority_woken == pdTRUE) {
            portYIELD_FROM_ISR(higher_priority_woken);
        }
    }

    [[nodiscard]] bool stop() noexcept {
        if (m_stopped) return false;
        m_stopped = true;
        portENTER_CRITICAL(&m_mutex);
        m_accepting = false;
        for (WorkRecord& work : m_work) {
            if (work.state != WorkState::free) {
                work.retirement_requested.store(true, std::memory_order_release);
            }
        }
        portEXIT_CRITICAL(&m_mutex);

        const TickType_t wait_ticks = detail::worker_shutdown_ticks(0);
        if (m_worker_started) {
            WorkerMessage message{};
            message.type = WorkerMessageType::stop;
            if (xQueueSend(m_worker_queue, &message, wait_ticks) != pdTRUE ||
                xSemaphoreTake(m_worker_done, wait_ticks) != pdTRUE) {
                // Queue storage, ISR endpoint pointers, and the static task all
                // live in State. Keep them reserved if the worker cannot retire.
                ESP_LOGE(kLogTag, "GPIO worker join timed out; retaining native resources");
                return false;
            }
            m_worker_started = false;
        }
        // The IDF GPIO ISR service is process-global and may have been installed
        // by another native user. Per-pin handlers are removed by stop_all();
        // retaining the one bounded service allocation avoids uninstalling a
        // service shared with another EspIdfGpioBackend or IDF component.
        delete_native_objects();
        return true;
    }


    std::array<EndpointState, kMaximumEndpoints> m_endpoints{};
    std::size_t m_endpoint_count = 0;
    std::array<WorkRecord, kMaximumOperationSlots> m_work{};
    std::array<std::uint8_t, kWorkerQueueLength * sizeof(WorkerMessage)> m_queue_storage{};
    StaticQueue_t m_queue_control{};
    QueueHandle_t m_worker_queue = nullptr;
    StaticSemaphore_t m_worker_done_control{};
    SemaphoreHandle_t m_worker_done = nullptr;
    alignas(StackType_t) std::array<std::uint8_t, kWorkerStackBytes> m_worker_stack{};
    StaticTask_t m_worker_control{};
    TaskHandle_t m_worker = nullptr;
    portMUX_TYPE m_mutex = portMUX_INITIALIZER_UNLOCKED;
    std::uint32_t m_next_interrupt_serial = 1;
    bool m_accepting = true;
    bool m_worker_started = false;
    bool m_owns_isr_service = false;
    bool m_stopped = false;
};

EspIdfGpioBackend::EspIdfGpioBackend(
    std::span<const EspGpioEndpointConfig> endpoints) {
    if (!endpoints.empty()) m_state = std::make_unique<State>(endpoints);
}

EspIdfGpioBackend::~EspIdfGpioBackend() {
    if (!m_state) return;
    if (m_state->stop()) m_state.reset();
    else (void)m_state.release();
}

std::span<const DeviceOperation> EspIdfGpioBackend::operations() const noexcept {
    return kImplementedOperations;
}

IoStatus EspIdfGpioBackend::start_operation(
    const BackendOperation& operation,
    std::weak_ptr<PeripheralCompletionSink> completions) {
    if (!m_state) return IoStatus::invalid_argument;
    return m_state->start(operation, std::move(completions));
}

IoStatus EspIdfGpioBackend::cancel_operation(const BackendOperation& operation) {
    if (!m_state) return IoStatus::invalid_argument;
    return m_state->cancel(operation);
}

void EspIdfGpioBackend::release_application(std::uint64_t owner_generation) {
    if (m_state) m_state->release(owner_generation);
}

EspIdfGpioRegistry::EspIdfGpioRegistry(
    PeripheralRegistry& registry, std::span<const EspGpioEndpointConfig> endpoints)
    : m_registry(registry) {
    if (endpoints.empty()) return;
    m_gpio = std::make_shared<EspIdfGpioBackend>(endpoints);
    for (const EspGpioEndpointConfig& endpoint : endpoints) {
        m_registry.register_endpoint(
            {endpoint.logical_name, esp_idf_gpio_version, false},
            EndpointAccess::application, m_gpio);
    }
}

} // namespace mb
