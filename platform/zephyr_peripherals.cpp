#include "platform/zephyr_peripherals.h"

#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace mb::zephyr {
namespace {

constexpr CapabilityVersion gpio_version{Capability::gpio, peripheral_abi_major, 0};

enum class SlotState : std::uint8_t { empty, queued, running, result_ready };

struct OperationTicket {
    DeviceHandle device = {};
    DeviceHandle operation = {};
    DeviceHandle subscription = {};
    std::uint64_t owner_generation = 0;
    DeviceOperation id = DeviceOperation::none;
    GpioDirection direction = GpioDirection::input;
    GpioPull pull = GpioPull::none;
    GpioEdge edge = GpioEdge::both;
    bool initial_high = false;
    bool high = false;
};

struct OperationResult {
    IoStatus status = IoStatus::io_failure;
    std::int64_t timestamp_ns = 0;
    bool high = false;
};

struct OperationSlot {
    OperationTicket ticket = {};
    OperationResult result = {};
    SlotState state = SlotState::empty;
    bool cancelled = false;
    bool retired = false;
};

struct IsrSample {
    DeviceHandle subscription = {};
    DeviceHandle device = {};
    std::uint64_t owner_generation = 0;
    std::int64_t timestamp_ns = 0;
    bool high = false;
};

struct SubscriptionState {
    DeviceHandle subscription = {};
    DeviceHandle device = {};
    std::uint64_t owner_generation = 0;
    bool reserved = false;
    bool callback_registered = false;
    bool interrupt_enabled = false;
    bool accept_events = false;
    bool ready = false;
};

static_assert(std::is_trivially_copyable_v<OperationTicket>);
static_assert(std::is_trivially_copyable_v<OperationResult>);
static_assert(std::is_trivially_copyable_v<OperationSlot>);
static_assert(std::is_trivially_copyable_v<IsrSample>);
static_assert(std::is_trivially_copyable_v<SubscriptionState>);

[[nodiscard]] OperationResult success() { return {IoStatus::completed}; }

[[nodiscard]] OperationResult failure(IoStatus status) { return {status}; }

[[nodiscard]] IoStatus status_from_errno(int error) noexcept {
    if (error >= 0) return IoStatus::completed;
    switch (-error) {
    case EINVAL: return IoStatus::invalid_argument;
    case EACCES:
    case EPERM: return IoStatus::permission_denied;
    case ENOTSUP:
    case ENOSYS: return IoStatus::unsupported;
    case EBUSY:
    case EAGAIN: return IoStatus::busy;
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK: return IoStatus::busy;
#endif
    case ETIMEDOUT: return IoStatus::timeout;
    case ENODEV:
    case ENXIO: return IoStatus::device_lost;
    case ENOMEM: return IoStatus::memory_exhausted;
    default: return IoStatus::io_failure;
    }
}

[[nodiscard]] OperationResult gpio_result(int error) {
    if (error < 0) return failure(status_from_errno(error));
    return success();
}

[[nodiscard]] std::optional<gpio_flags_t> pull_flags(GpioPull pull) noexcept {
    switch (pull) {
    case GpioPull::none: return gpio_flags_t{0};
    case GpioPull::up: return gpio_flags_t{GPIO_PULL_UP};
    case GpioPull::down: return gpio_flags_t{GPIO_PULL_DOWN};
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<gpio_flags_t> edge_flags(GpioEdge edge,
                                                     const gpio_dt_spec& spec) noexcept {
    switch (edge) {
    case GpioEdge::rising:
        return (spec.dt_flags & GPIO_ACTIVE_LOW) != 0U
                   ? gpio_flags_t{GPIO_INT_EDGE_TO_ACTIVE}
                   : gpio_flags_t{GPIO_INT_EDGE_RISING};
    case GpioEdge::falling:
        return (spec.dt_flags & GPIO_ACTIVE_LOW) != 0U
                   ? gpio_flags_t{GPIO_INT_EDGE_TO_INACTIVE}
                   : gpio_flags_t{GPIO_INT_EDGE_FALLING};
    case GpioEdge::both: return gpio_flags_t{GPIO_INT_EDGE_BOTH};
    }
    return std::nullopt;
}

[[nodiscard]] std::int64_t uptime_ns() noexcept {
    return static_cast<std::int64_t>(k_ticks_to_ns_floor64(k_uptime_ticks()));
}

[[nodiscard]] bool contains(std::span<const DeviceOperation> operations,
                            DeviceOperation operation) noexcept {
    return std::find(operations.begin(), operations.end(), operation) != operations.end();
}

[[nodiscard]] bool request_matches(DeviceOperation operation, const DeviceRequest& request) {
    switch (operation) {
    case DeviceOperation::open: return std::holds_alternative<OpenRequest>(request);
    case DeviceOperation::configure: return std::holds_alternative<GpioConfigureRequest>(request);
    case DeviceOperation::read: return std::holds_alternative<GpioReadRequest>(request);
    case DeviceOperation::write: return std::holds_alternative<GpioWriteRequest>(request);
    case DeviceOperation::subscribe: return std::holds_alternative<GpioSubscribeRequest>(request);
    case DeviceOperation::unsubscribe: return std::holds_alternative<UnsubscribeRequest>(request);
    case DeviceOperation::close: return std::holds_alternative<CloseRequest>(request);
    default: return false;
    }
}

void validate_configuration(const ZephyrGpioConfig& config) {
    if (config.logical_name.empty() ||
        config.logical_name.size() > gpio_max_logical_name_bytes) {
        throw std::invalid_argument("invalid Zephyr GPIO logical name");
    }
    if (config.gpio.port == nullptr || config.gpio.pin >= GPIO_MAX_PINS_PER_PORT) {
        throw std::invalid_argument("invalid Zephyr GPIO devicetree specification");
    }
    if ((config.gpio.dt_flags & (GPIO_PULL_UP | GPIO_PULL_DOWN)) ==
        (GPIO_PULL_UP | GPIO_PULL_DOWN)) {
        throw std::invalid_argument("conflicting Zephyr GPIO devicetree pulls");
    }
    switch (config.direction) {
    case GpioDirectionPermission::input_only:
    case GpioDirectionPermission::output_only:
    case GpioDirectionPermission::input_and_output: break;
    default: throw std::invalid_argument("invalid Zephyr GPIO direction permission");
    }
    if (config.operations.empty() ||
        config.operations.size() > gpio_max_pending_operations) {
        throw std::invalid_argument("invalid Zephyr GPIO operation list size");
    }

    for (std::size_t index = 0; index < config.operations.size(); ++index) {
        const DeviceOperation operation = config.operations[index];
        if (contains(config.operations.subspan(0, index), operation)) {
            throw std::invalid_argument("duplicate Zephyr GPIO operation");
        }
        switch (operation) {
        case DeviceOperation::open:
        case DeviceOperation::configure:
        case DeviceOperation::read:
        case DeviceOperation::write:
        case DeviceOperation::subscribe:
        case DeviceOperation::unsubscribe:
        case DeviceOperation::close:
        case DeviceOperation::cancel: break;
        default: throw std::invalid_argument("operation is not implemented by Zephyr GPIO");
        }
    }
    if (!contains(config.operations, DeviceOperation::open) ||
        !contains(config.operations, DeviceOperation::close)) {
        throw std::invalid_argument("Zephyr GPIO requires open and close operations");
    }
    if (config.direction == GpioDirectionPermission::input_only &&
        contains(config.operations, DeviceOperation::write)) {
        throw std::invalid_argument("an input-only GPIO cannot advertise write");
    }
    if (config.direction == GpioDirectionPermission::output_only &&
        contains(config.operations, DeviceOperation::subscribe)) {
        throw std::invalid_argument("an output-only GPIO cannot advertise subscribe");
    }
    if (contains(config.operations, DeviceOperation::unsubscribe) &&
        !contains(config.operations, DeviceOperation::subscribe)) {
        throw std::invalid_argument("Zephyr GPIO unsubscribe requires subscribe");
    }
    if (config.direction == GpioDirectionPermission::input_only &&
        contains(config.operations, DeviceOperation::configure) &&
        (config.gpio.dt_flags & GPIO_SINGLE_ENDED) != 0U) {
        throw std::invalid_argument("an open-drain/source input cannot be configured as input only");
    }
    if (!gpio_is_ready_dt(&config.gpio)) {
        throw std::invalid_argument("Zephyr GPIO controller is not ready");
    }
}

} // namespace

struct ZephyrGpioBackend::State {
    struct UiOperationSlot {
        BackendOperation operation;
        std::weak_ptr<PeripheralCompletionSink> completions;
    };

    struct UiSubscriptionState {
        DeviceHandle subscription = {};
        DeviceHandle device = {};
        std::uint64_t owner_generation = 0;
        std::weak_ptr<PeripheralCompletionSink> completions;
        bool reserved = false;
    };

    State(k_work_q& queue, const ZephyrGpioConfig& config)
        : application_thread(k_current_get()), work_queue(&queue), gpio(config.gpio),
          direction_permission(config.direction), logical_name_size(config.logical_name.size()) {
        validate_configuration(config);
        if (k_work_queue_thread_get(work_queue) == nullptr) {
            throw std::invalid_argument("Zephyr GPIO work queue is not started");
        }
        std::copy(config.logical_name.begin(), config.logical_name.end(), logical_name.begin());
        std::copy(config.operations.begin(), config.operations.end(), implemented.begin());
        implemented_count = config.operations.size();
        k_work_init(&work_item, &State::run_work);
        gpio_init_callback(&callback, &State::on_gpio, BIT(gpio.pin));
    }

    void assert_application_thread() const noexcept {
        if (k_is_in_isr() || k_current_get() != application_thread) {
            k_oops();
        }
    }

    [[nodiscard]] std::string_view name() const noexcept {
        return {logical_name.data(), logical_name_size};
    }

    [[nodiscard]] std::span<const DeviceOperation> operation_list() const noexcept {
        return {implemented.data(), implemented_count};
    }

    [[nodiscard]] bool supports(DeviceOperation id) const noexcept {
        return contains(operation_list(), id);
    }

    [[nodiscard]] bool allows(GpioDirection direction) const noexcept {
        switch (direction_permission) {
        case GpioDirectionPermission::input_only: return direction == GpioDirection::input;
        case GpioDirectionPermission::output_only: return direction == GpioDirection::output;
        case GpioDirectionPermission::input_and_output: return true;
        }
        return false;
    }

    [[nodiscard]] IoStatus validate_common(const BackendOperation& operation) const noexcept {
        if (operation.logical_name != name() || operation.owner_generation == 0 ||
            operation.budget.maximum_open_handles == 0 ||
            operation.budget.maximum_open_handles > gpio_max_open_handles ||
            operation.budget.maximum_pending_operations == 0 ||
            operation.budget.maximum_pending_operations > gpio_max_pending_operations ||
            operation.budget.maximum_bytes_per_buffer > gpio_max_bytes_per_buffer) {
            return IoStatus::invalid_argument;
        }
        if (!supports(operation.id) || operation.id == DeviceOperation::cancel ||
            !request_matches(operation.id, operation.request)) {
            return IoStatus::invalid_argument;
        }
        if (operation.id == DeviceOperation::open) {
            if (!operation.device.is_empty()) return IoStatus::invalid_argument;
            const auto& request = std::get<OpenRequest>(operation.request);
            if (request.minimum_version.capability != Capability::gpio ||
                request.minimum_version.major != peripheral_abi_major) {
                return IoStatus::invalid_argument;
            }
            if (request.minimum_version.major > gpio_version.major ||
                request.minimum_version.minor > gpio_version.minor) {
                return IoStatus::unsupported;
            }
            return IoStatus::completed;
        }
        if (operation.device.is_empty() || operation.device.capability != Capability::gpio ||
            operation.device.owner_generation != operation.owner_generation) {
            return IoStatus::invalid_argument;
        }
        if (operation.id == DeviceOperation::configure) {
            const auto& request = std::get<GpioConfigureRequest>(operation.request);
            if (!allows(request.direction)) return IoStatus::permission_denied;
            if (!pull_flags(request.pull)) return IoStatus::invalid_argument;
            if (request.direction == GpioDirection::input &&
                (gpio.dt_flags & GPIO_SINGLE_ENDED) != 0U) {
                return IoStatus::invalid_argument;
            }
        } else if (operation.id == DeviceOperation::subscribe) {
            const auto& request = std::get<GpioSubscribeRequest>(operation.request);
            if (direction_permission == GpioDirectionPermission::output_only) {
                return IoStatus::permission_denied;
            }
            if (!edge_flags(request.edge, gpio) || operation.subscription.is_empty() ||
                operation.subscription.capability != Capability::gpio ||
                operation.subscription.owner_generation != operation.owner_generation) {
                return IoStatus::invalid_argument;
            }
        } else if (operation.id == DeviceOperation::write) {
            if (direction_permission == GpioDirectionPermission::input_only) {
                return IoStatus::permission_denied;
            }
        }
        return IoStatus::completed;
    }

    [[nodiscard]] OperationTicket ticket_from(const BackendOperation& operation) const {
        OperationTicket ticket;
        ticket.device = operation.device;
        ticket.operation = operation.operation;
        ticket.subscription = operation.subscription;
        ticket.owner_generation = operation.owner_generation;
        ticket.id = operation.id;
        switch (operation.id) {
        case DeviceOperation::configure: {
            const auto& request = std::get<GpioConfigureRequest>(operation.request);
            ticket.direction = request.direction;
            ticket.pull = request.pull;
            ticket.initial_high = request.initial_high;
            break;
        }
        case DeviceOperation::write:
            ticket.high = std::get<GpioWriteRequest>(operation.request).high;
            break;
        case DeviceOperation::subscribe:
            ticket.edge = std::get<GpioSubscribeRequest>(operation.request).edge;
            break;
        case DeviceOperation::unsubscribe:
            ticket.subscription = std::get<UnsubscribeRequest>(operation.request).subscription;
            break;
        default: break;
        }
        return ticket;
    }

    [[nodiscard]] IoStatus validate_state_locked(const BackendOperation& operation,
                                                 const OperationTicket& ticket) const noexcept {
        if (operation.id == DeviceOperation::subscribe) {
            if (subscription.reserved || background_stop_pending) return IoStatus::busy;
            if (direction_permission == GpioDirectionPermission::input_and_output &&
                direction_configured && configured_direction == GpioDirection::output) {
                return IoStatus::permission_denied;
            }
        } else if (operation.id == DeviceOperation::write &&
                   direction_permission == GpioDirectionPermission::input_and_output &&
                   (!direction_configured || configured_direction != GpioDirection::output)) {
            return IoStatus::permission_denied;
        } else if (operation.id == DeviceOperation::unsubscribe) {
            if (!subscription.reserved || ticket.subscription != subscription.subscription ||
                subscription.owner_generation != operation.owner_generation ||
                subscription.device != operation.device) {
                return IoStatus::invalid_argument;
            }
        }
        return IoStatus::completed;
    }

    void retire_device_operations_locked(const BackendOperation& close,
                                         std::size_t accepted_index) {
        for (std::size_t index = 0; index < slots.size(); ++index) {
            if (index == accepted_index) continue;
            auto& slot = slots[index];
            if (slot.state == SlotState::empty) continue;
            if (slot.ticket.owner_generation == close.owner_generation &&
                slot.ticket.device == close.device) {
                slot.cancelled = true;
                slot.retired = true;
            }
        }
    }

    [[nodiscard]] bool prepare_control_locked(const BackendOperation& operation,
                                              std::size_t accepted_index) {
        if (operation.id == DeviceOperation::close) {
            retire_device_operations_locked(operation, accepted_index);
            if (subscription.reserved &&
                subscription.owner_generation == operation.owner_generation &&
                subscription.device == operation.device) {
                subscription.accept_events = false;
                subscription.ready = false;
                return true;
            }
        } else if (operation.id == DeviceOperation::unsubscribe) {
            subscription.accept_events = false;
            subscription.ready = false;
            return true;
        }
        return false;
    }

    [[nodiscard]] IoStatus submission_status(int result) const noexcept {
        if (result >= 0) return IoStatus::completed;
        if (result == -EBUSY) return IoStatus::busy;
        return IoStatus::device_lost;
    }

    void release_ticket_locked(std::size_t index) {
        slots[index] = OperationSlot{};
        --pending_count;
    }

    IoStatus start(const BackendOperation& operation,
                   std::weak_ptr<PeripheralCompletionSink> completions) {
        const IoStatus common_status = validate_common(operation);
        if (common_status != IoStatus::completed) return common_status;
        const OperationTicket ticket = ticket_from(operation);

        const auto key = k_spin_lock(&lock);
        if (shutting_down) {
            k_spin_unlock(&lock, key);
            return IoStatus::device_lost;
        }
        IoStatus status = validate_state_locked(operation, ticket);
        if (status != IoStatus::completed) {
            k_spin_unlock(&lock, key);
            return status;
        }
        if (pending_count == slots.size()) {
            k_spin_unlock(&lock, key);
            return IoStatus::busy;
        }

        std::size_t slot_index = 0;
        while (slots[slot_index].state != SlotState::empty) ++slot_index;
        auto& slot = slots[slot_index];
        slot.ticket = ticket;
        slot.state = SlotState::queued;
        operation_queue[(operation_head + operation_count) % operation_queue.size()] =
            static_cast<std::uint8_t>(slot_index);
        ++operation_count;
        ++pending_count;
        status = submission_status(k_work_submit_to_queue(work_queue, &work_item));
        if (status != IoStatus::completed) {
            --operation_count;
            release_ticket_locked(slot_index);
            k_spin_unlock(&lock, key);
            return status;
        }

        const bool clear_subscription_reference =
            prepare_control_locked(operation, slot_index);
        if (operation.id == DeviceOperation::subscribe) {
            subscription.subscription = ticket.subscription;
            subscription.device = ticket.device;
            subscription.owner_generation = ticket.owner_generation;
            subscription.reserved = true;
            subscription.accept_events = true;
            subscription.ready = false;
            sample_head = 0;
            sample_count = 0;
        }
        k_spin_unlock(&lock, key);

        if (clear_subscription_reference) ui_subscription = UiSubscriptionState{};
        ui_slots[slot_index].operation = operation;
        ui_slots[slot_index].completions = std::move(completions);
        if (operation.id == DeviceOperation::subscribe) {
            ui_subscription.subscription = ticket.subscription;
            ui_subscription.device = ticket.device;
            ui_subscription.owner_generation = ticket.owner_generation;
            ui_subscription.completions = ui_slots[slot_index].completions;
            ui_subscription.reserved = true;
        }
        return status;
    }

    IoStatus cancel(const BackendOperation& operation) {
        OperationTicket cancelled_ticket;
        bool clear_subscription_reference = false;
        const auto key = k_spin_lock(&lock);
        for (auto& slot : slots) {
            if (slot.state == SlotState::empty ||
                slot.ticket.operation != operation.operation ||
                slot.ticket.owner_generation != operation.owner_generation) {
                continue;
            }
            if (slot.cancelled || slot.retired) {
                k_spin_unlock(&lock, key);
                return IoStatus::invalid_argument;
            }
            slot.cancelled = true;
            if (subscription_is_current_locked(slot.ticket)) {
                subscription.accept_events = false;
                subscription.ready = false;
                background_stop_pending = true;
                cancelled_ticket = slot.ticket;
                clear_subscription_reference = true;
            }
            if (!shutting_down) (void)k_work_submit_to_queue(work_queue, &work_item);
            k_spin_unlock(&lock, key);
            if (clear_subscription_reference) {
                clear_ui_subscription_if_matches(cancelled_ticket.subscription,
                                                 cancelled_ticket.device,
                                                 cancelled_ticket.owner_generation);
            }
            return IoStatus::completed;
        }
        k_spin_unlock(&lock, key);
        return IoStatus::invalid_argument;
    }

    void release(std::uint64_t owner_generation) {
        const auto key = k_spin_lock(&lock);
        bool work_needed = false;
        for (auto& slot : slots) {
            if (slot.state != SlotState::empty &&
                slot.ticket.owner_generation == owner_generation) {
                slot.cancelled = true;
                slot.retired = true;
                work_needed = true;
            }
        }
        bool clear_subscription_reference = false;
        if (subscription.reserved && subscription.owner_generation == owner_generation) {
            subscription.accept_events = false;
            subscription.ready = false;
            background_stop_pending = true;
            clear_subscription_reference = true;
        }
        if (background_stop_pending) work_needed = true;
        if (work_needed && !shutting_down) {
            (void)k_work_submit_to_queue(work_queue, &work_item);
        }
        k_spin_unlock(&lock, key);
        if (clear_subscription_reference && ui_subscription.reserved &&
            ui_subscription.owner_generation == owner_generation) {
            ui_subscription = UiSubscriptionState{};
        }
    }

    [[nodiscard]] bool subscription_matches_locked(const OperationTicket& ticket) const noexcept {
        return subscription.reserved && subscription.subscription == ticket.subscription &&
               subscription.device == ticket.device &&
               subscription.owner_generation == ticket.owner_generation;
    }

    [[nodiscard]] bool subscription_is_current_locked(
        const OperationTicket& ticket) const noexcept {
        return ticket.id == DeviceOperation::subscribe && subscription_matches_locked(ticket);
    }

    [[nodiscard]] bool continue_subscription(const OperationTicket& ticket) {
        const auto key = k_spin_lock(&lock);
        const bool proceed =
            subscription_is_current_locked(ticket) && subscription.accept_events;
        k_spin_unlock(&lock, key);
        return proceed;
    }

    void clear_subscription_locked() {
        subscription.subscription = {};
        subscription.device = {};
        subscription.owner_generation = 0;
        subscription.reserved = false;
        subscription.accept_events = false;
        subscription.ready = false;
        sample_head = 0;
        sample_count = 0;
    }

    IoStatus stop_subscription() {
        {
            const auto key = k_spin_lock(&lock);
            subscription.accept_events = false;
            subscription.ready = false;
            if (!subscription.reserved && !subscription.callback_registered &&
                !subscription.interrupt_enabled) {
                sample_head = 0;
                sample_count = 0;
                k_spin_unlock(&lock, key);
                return IoStatus::completed;
            }
            k_spin_unlock(&lock, key);
        }

        int disable_result = 0;
        int remove_result = 0;
        {
            const auto key = k_spin_lock(&lock);
            const bool disable = subscription.interrupt_enabled;
            const bool remove = subscription.callback_registered;
            k_spin_unlock(&lock, key);
            if (disable) {
                disable_result = gpio_pin_interrupt_configure_dt(&gpio, GPIO_INT_DISABLE);
            }
            if (remove) remove_result = gpio_remove_callback_dt(&gpio, &callback);
        }

        const auto key = k_spin_lock(&lock);
        if (disable_result == 0) subscription.interrupt_enabled = false;
        if (remove_result == 0) subscription.callback_registered = false;
        if (!subscription.interrupt_enabled && !subscription.callback_registered) {
            clear_subscription_locked();
        }
        k_spin_unlock(&lock, key);
        if (disable_result < 0) return status_from_errno(disable_result);
        if (remove_result < 0) return status_from_errno(remove_result);
        return IoStatus::completed;
    }

    OperationResult execute_open() {
        if (!gpio_is_ready_dt(&gpio)) return failure(IoStatus::device_lost);
        return success();
    }

    OperationResult execute_configure(const OperationTicket& ticket) {
        {
            const auto key = k_spin_lock(&lock);
            if (subscription.reserved || background_stop_pending) {
                k_spin_unlock(&lock, key);
                return failure(IoStatus::busy);
            }
            k_spin_unlock(&lock, key);
        }
        const auto pulls = pull_flags(ticket.pull);
        if (!pulls || !allows(ticket.direction)) return failure(IoStatus::invalid_argument);
        if (ticket.direction == GpioDirection::input &&
            (gpio.dt_flags & GPIO_SINGLE_ENDED) != 0U) {
            return failure(IoStatus::invalid_argument);
        }

        gpio_flags_t flags = *pulls;
        if (ticket.direction == GpioDirection::input) {
            flags |= GPIO_INPUT;
        } else {
            flags |= ticket.initial_high ? GPIO_OUTPUT_ACTIVE : GPIO_OUTPUT_INACTIVE;
        }
        gpio_dt_spec adjusted = gpio;
        adjusted.dt_flags &= ~(GPIO_PULL_UP | GPIO_PULL_DOWN);
        const int result = gpio_pin_configure_dt(&adjusted, flags);
        if (result < 0) return failure(status_from_errno(result));
        const auto key = k_spin_lock(&lock);
        configured_direction = ticket.direction;
        direction_configured = true;
        k_spin_unlock(&lock, key);
        return success();
    }

    OperationResult execute_read() {
        const int value = gpio_pin_get_dt(&gpio);
        if (value < 0) return failure(status_from_errno(value));
        OperationResult result = success();
        result.high = value != 0;
        result.timestamp_ns = uptime_ns();
        return result;
    }

    OperationResult execute_write(const OperationTicket& ticket) {
        {
            const auto key = k_spin_lock(&lock);
            const bool allowed = direction_permission == GpioDirectionPermission::output_only ||
                                 (direction_permission ==
                                      GpioDirectionPermission::input_and_output &&
                                  direction_configured &&
                                  configured_direction == GpioDirection::output);
            k_spin_unlock(&lock, key);
            if (!allowed) return failure(IoStatus::permission_denied);
        }
        return gpio_result(gpio_pin_set_dt(&gpio, ticket.high ? 1 : 0));
    }

    OperationResult execute_subscribe(const OperationTicket& ticket) {
        if (!continue_subscription(ticket)) return failure(IoStatus::cancelled);
        const auto trigger = edge_flags(ticket.edge, gpio);
        if (!trigger) return failure(IoStatus::invalid_argument);

        int result = gpio_add_callback_dt(&gpio, &callback);
        if (result < 0) return failure(status_from_errno(result));
        {
            const auto key = k_spin_lock(&lock);
            subscription.callback_registered = true;
            k_spin_unlock(&lock, key);
        }
        if (!continue_subscription(ticket)) {
            (void)stop_subscription();
            return failure(IoStatus::cancelled);
        }

        result = gpio_pin_interrupt_configure_dt(&gpio, *trigger);
        {
            const auto key = k_spin_lock(&lock);
            subscription.interrupt_enabled = result == 0;
            k_spin_unlock(&lock, key);
        }
        if (result < 0) {
            (void)stop_subscription();
            return failure(status_from_errno(result));
        }
        if (!continue_subscription(ticket)) {
            (void)stop_subscription();
            return failure(IoStatus::cancelled);
        }
        return success();
    }

    OperationResult execute_unsubscribe(const OperationTicket& ticket) {
        const auto key = k_spin_lock(&lock);
        const bool matches = subscription_matches_locked(ticket);
        k_spin_unlock(&lock, key);
        if (!matches) return failure(IoStatus::invalid_argument);
        const IoStatus status = stop_subscription();
        return status == IoStatus::completed ? success() : failure(status);
    }

    OperationResult execute_close(const OperationTicket& ticket) {
        const auto key = k_spin_lock(&lock);
        const bool matches = ticket.id == DeviceOperation::close && subscription.reserved &&
                             subscription.owner_generation == ticket.owner_generation &&
                             subscription.device == ticket.device;
        k_spin_unlock(&lock, key);
        if (!matches) return success();
        const IoStatus status = stop_subscription();
        return status == IoStatus::completed ? success() : failure(status);
    }

    OperationResult execute(const OperationTicket& ticket) {
        switch (ticket.id) {
        case DeviceOperation::open: return execute_open();
        case DeviceOperation::configure: return execute_configure(ticket);
        case DeviceOperation::read: return execute_read();
        case DeviceOperation::write: return execute_write(ticket);
        case DeviceOperation::subscribe: return execute_subscribe(ticket);
        case DeviceOperation::unsubscribe: return execute_unsubscribe(ticket);
        case DeviceOperation::close: return execute_close(ticket);
        default: return failure(IoStatus::unsupported);
        }
    }

    void finish_operation(std::size_t index, const OperationTicket& ticket,
                          OperationResult result) {
        const auto key = k_spin_lock(&lock);
        auto& slot = slots[index];
        if (slot.state != SlotState::running || slot.ticket.operation != ticket.operation ||
            slot.ticket.owner_generation != ticket.owner_generation || slot.ticket.id != ticket.id) {
            k_spin_unlock(&lock, key);
            return;
        }
        slot.result = result;
        slot.state = SlotState::result_ready;
        result_queue[(result_head + result_count) % result_queue.size()] =
            static_cast<std::uint8_t>(index);
        ++result_count;
        if (ticket.id == DeviceOperation::subscribe && result.status != IoStatus::completed &&
            subscription_is_current_locked(ticket)) {
            if (!subscription.callback_registered && !subscription.interrupt_enabled) {
                clear_subscription_locked();
            } else {
                subscription.accept_events = false;
                subscription.ready = false;
                background_stop_pending = true;
            }
        }
        k_spin_unlock(&lock, key);
    }

    void process_operation() {
        std::size_t index = slots.size();
        OperationTicket ticket;
        bool cancelled = false;
        bool retired = false;
        {
            const auto key = k_spin_lock(&lock);
            if (operation_count != 0) {
                index = operation_queue[operation_head];
                operation_head = (operation_head + 1) % operation_queue.size();
                --operation_count;
                auto& slot = slots[index];
                slot.state = SlotState::running;
                ticket = slot.ticket;
                cancelled = slot.cancelled;
                retired = slot.retired;
            }
            k_spin_unlock(&lock, key);
        }
        if (index == slots.size()) return;

        const bool control = ticket.id == DeviceOperation::close ||
                             ticket.id == DeviceOperation::unsubscribe;
        OperationResult result;
        if (!control && (cancelled || retired)) {
            result = failure(IoStatus::cancelled);
        } else {
            result = execute(ticket);
            if (cancelled) result = failure(IoStatus::cancelled);
        }
        finish_operation(index, ticket, result);
    }

    void service_background_stop() {
        {
            const auto key = k_spin_lock(&lock);
            if (!background_stop_pending) {
                k_spin_unlock(&lock, key);
                return;
            }
            background_stop_pending = false;
            k_spin_unlock(&lock, key);
        }
        (void)stop_subscription();
    }

    [[nodiscard]] bool ui_subscription_matches(const DeviceHandle& handle,
                                               const DeviceHandle& device_handle,
                                               std::uint64_t owner_generation) const noexcept {
        return ui_subscription.reserved && ui_subscription.subscription == handle &&
               ui_subscription.device == device_handle &&
               ui_subscription.owner_generation == owner_generation;
    }

    void clear_ui_subscription_if_matches(const DeviceHandle& handle,
                                          const DeviceHandle& device_handle,
                                          std::uint64_t owner_generation) {
        if (ui_subscription_matches(handle, device_handle, owner_generation)) {
            ui_subscription = UiSubscriptionState{};
        }
    }

    void finish_subscription_delivery(const OperationTicket& ticket, IoStatus status,
                                      bool delivered) {
        bool clear_reference = false;
        const auto key = k_spin_lock(&lock);
        if (subscription_is_current_locked(ticket)) {
            if (status == IoStatus::completed && delivered && subscription.accept_events) {
                subscription.ready = true;
            } else if (!subscription.callback_registered && !subscription.interrupt_enabled) {
                clear_subscription_locked();
                clear_reference = true;
            } else {
                subscription.accept_events = false;
                subscription.ready = false;
                background_stop_pending = true;
                clear_reference = true;
                if (!shutting_down) (void)k_work_submit_to_queue(work_queue, &work_item);
            }
        } else {
            clear_reference = true;
        }
        k_spin_unlock(&lock, key);
        if (clear_reference) {
            clear_ui_subscription_if_matches(ticket.subscription, ticket.device,
                                             ticket.owner_generation);
        }
    }

    [[nodiscard]] DeviceResponse response_for(const BackendOperation& operation,
                                              const OperationResult& result) const {
        if (result.status != IoStatus::completed) return std::monostate{};
        switch (operation.id) {
        case DeviceOperation::open: return OpenResponse{{}, gpio_version, false};
        case DeviceOperation::configure:
        case DeviceOperation::write:
        case DeviceOperation::unsubscribe:
        case DeviceOperation::close: return EmptyResponse{};
        case DeviceOperation::read: return GpioReadResponse{result.high, result.timestamp_ns};
        case DeviceOperation::subscribe: return SubscriptionResponse{operation.subscription};
        default: return std::monostate{};
        }
    }

    void complete_ready_operations() {
        for (std::size_t count = 0; count < slots.size(); ++count) {
            std::size_t index = slots.size();
            OperationTicket ticket;
            OperationResult result;
            bool cancelled = false;
            bool retired = false;
            {
                const auto key = k_spin_lock(&lock);
                if (result_count != 0) {
                    index = result_queue[result_head];
                    result_head = (result_head + 1) % result_queue.size();
                    --result_count;
                    const auto& slot = slots[index];
                    ticket = slot.ticket;
                    result = slot.result;
                    cancelled = slot.cancelled;
                    retired = slot.retired;
                    release_ticket_locked(index);
                }
                k_spin_unlock(&lock, key);
            }
            if (index == slots.size()) return;

            BackendOperation operation = std::move(ui_slots[index].operation);
            auto completion_reference = std::move(ui_slots[index].completions);
            ui_slots[index] = UiOperationSlot{};
            const auto completion = completion_reference.lock();
            if (cancelled) result = failure(IoStatus::cancelled);
            const bool valid = operation.id == ticket.id &&
                               operation.operation == ticket.operation &&
                               operation.owner_generation == ticket.owner_generation;
            const bool delivered = completion && !retired && valid;
            if (ticket.id == DeviceOperation::subscribe) {
                finish_subscription_delivery(ticket, result.status, delivered);
            }
            if (delivered) {
                completion->complete(operation, result.status, response_for(operation, result));
            }
        }
    }

    void synchronize_ui_subscription() {
        if (!ui_subscription.reserved) return;
        const auto key = k_spin_lock(&lock);
        const bool current = subscription.reserved &&
                             subscription.subscription == ui_subscription.subscription &&
                             subscription.device == ui_subscription.device &&
                             subscription.owner_generation == ui_subscription.owner_generation;
        k_spin_unlock(&lock, key);
        if (!current) ui_subscription = UiSubscriptionState{};
    }

    void reject_notification_delivery(const IsrSample& sample) {
        const auto key = k_spin_lock(&lock);
        if (subscription.reserved && subscription.subscription == sample.subscription &&
            subscription.device == sample.device &&
            subscription.owner_generation == sample.owner_generation) {
            subscription.accept_events = false;
            subscription.ready = false;
            background_stop_pending = true;
            if (!shutting_down) (void)k_work_submit_to_queue(work_queue, &work_item);
        }
        k_spin_unlock(&lock, key);
        clear_ui_subscription_if_matches(sample.subscription, sample.device,
                                         sample.owner_generation);
    }

    void deliver_samples() {
        for (std::size_t count = 0; count < samples.size(); ++count) {
            IsrSample sample;
            {
                const auto key = k_spin_lock(&lock);
                if (sample_count == 0) {
                    k_spin_unlock(&lock, key);
                    return;
                }
                if (!subscription.reserved || !subscription.ready) {
                    if (!subscription.accept_events) {
                        sample_head = 0;
                        sample_count = 0;
                    }
                    k_spin_unlock(&lock, key);
                    return;
                }
                sample = samples[sample_head];
                sample_head = (sample_head + 1) % samples.size();
                --sample_count;
                if (sample.subscription != subscription.subscription ||
                    sample.device != subscription.device ||
                    sample.owner_generation != subscription.owner_generation) {
                    k_spin_unlock(&lock, key);
                    continue;
                }
                k_spin_unlock(&lock, key);
            }

            if (!ui_subscription_matches(sample.subscription, sample.device,
                                         sample.owner_generation)) {
                reject_notification_delivery(sample);
                continue;
            }
            const auto completion = ui_subscription.completions.lock();
            if (!completion) {
                reject_notification_delivery(sample);
                continue;
            }
            const IoStatus status = completion->publish(
                sample.owner_generation, sample.subscription,
                DeviceResponse{GpioReadResponse{sample.high, sample.timestamp_ns}});
            if (status == IoStatus::completed || status == IoStatus::busy ||
                status == IoStatus::memory_exhausted) {
                continue;
            }
            reject_notification_delivery(sample);
        }
    }

    void poll() {
        complete_ready_operations();
        synchronize_ui_subscription();
        deliver_samples();
    }

    void work() {
        service_background_stop();
        process_operation();
        service_background_stop();
        const auto key = k_spin_lock(&lock);
        const bool more_work = operation_count != 0 || background_stop_pending;
        if (more_work && !shutting_down) {
            (void)k_work_submit_to_queue(work_queue, &work_item);
        }
        k_spin_unlock(&lock, key);
    }

    IoStatus shutdown() noexcept {
        {
            const auto key = k_spin_lock(&lock);
            shutting_down = true;
            subscription.accept_events = false;
            subscription.ready = false;
            for (auto& slot : slots) {
                if (slot.state != SlotState::empty) {
                    slot.cancelled = true;
                    slot.retired = true;
                }
            }
            k_spin_unlock(&lock, key);
        }
        k_work_sync sync;
        (void)k_work_cancel_sync(&work_item, &sync);
        const IoStatus status = stop_subscription();

        for (auto& slot : ui_slots) slot = UiOperationSlot{};
        ui_subscription = UiSubscriptionState{};
        const auto key = k_spin_lock(&lock);
        for (auto& slot : slots) slot = OperationSlot{};
        operation_head = 0;
        operation_count = 0;
        pending_count = 0;
        result_head = 0;
        result_count = 0;
        sample_head = 0;
        sample_count = 0;
        background_stop_pending = false;
        k_spin_unlock(&lock, key);
        return status;
    }

    static void run_work(k_work* work_item) {
        State* self = CONTAINER_OF(work_item, State, work_item);
        self->work();
    }

    static void on_gpio(const device* port, gpio_callback* callback,
                        gpio_port_pins_t pins) {
        (void)port;
        State* self = CONTAINER_OF(callback, State, callback);
        IsrSample sample{};
        {
            const auto key = k_spin_lock(&self->lock);
            const bool capture = self->subscription.reserved &&
                                 self->subscription.accept_events &&
                                 self->subscription.callback_registered &&
                                 self->subscription.interrupt_enabled;
            if (capture) {
                sample.subscription = self->subscription.subscription;
                sample.device = self->subscription.device;
                sample.owner_generation = self->subscription.owner_generation;
            }
            k_spin_unlock(&self->lock, key);
            if (!capture) return;
        }

        // The Nordic on-chip implementation is a direct input-register read.
        // Controllers that can sleep must not enable subscriptions here.
        const int raw = gpio_pin_get_raw(self->gpio.port, self->gpio.pin);
        if (raw < 0) return;
        sample.timestamp_ns = uptime_ns();
        sample.high = (raw != 0) != ((self->gpio.dt_flags & GPIO_ACTIVE_LOW) != 0U);

        const auto key = k_spin_lock(&self->lock);
        if (!self->subscription.reserved || !self->subscription.accept_events ||
            sample.subscription != self->subscription.subscription ||
            sample.device != self->subscription.device ||
            sample.owner_generation != self->subscription.owner_generation) {
            k_spin_unlock(&self->lock, key);
            return;
        }
        if (self->sample_count == self->samples.size()) {
            self->sample_head = (self->sample_head + 1) % self->samples.size();
            --self->sample_count;
        }
        self->samples[(self->sample_head + self->sample_count) % self->samples.size()] = sample;
        ++self->sample_count;
        (void)k_work_submit_to_queue(self->work_queue, &self->work_item);
        k_spin_unlock(&self->lock, key);
    }

    k_tid_t application_thread;
    k_work_q* work_queue;
    gpio_dt_spec gpio;
    GpioDirectionPermission direction_permission;
    std::array<char, gpio_max_logical_name_bytes + 1> logical_name{};
    std::size_t logical_name_size;
    std::array<DeviceOperation, gpio_max_pending_operations> implemented{};
    std::size_t implemented_count = 0;
    GpioDirection configured_direction = GpioDirection::input;
    bool direction_configured = false;

    k_spinlock lock{};
    k_work work_item{};
    gpio_callback callback{};
    std::array<OperationSlot, gpio_max_pending_operations> slots{};
    std::array<UiOperationSlot, gpio_max_pending_operations> ui_slots{};
    std::array<std::uint8_t, gpio_max_pending_operations> operation_queue{};
    std::size_t operation_head = 0;
    std::size_t operation_count = 0;
    std::size_t pending_count = 0;
    std::array<std::uint8_t, gpio_max_pending_operations> result_queue{};
    std::size_t result_head = 0;
    std::size_t result_count = 0;
    std::array<IsrSample, gpio_max_notification_samples> samples{};
    std::size_t sample_head = 0;
    std::size_t sample_count = 0;
    SubscriptionState subscription{};
    UiSubscriptionState ui_subscription{};
    bool background_stop_pending = false;
    bool shutting_down = false;
};

ZephyrGpioBackend::ZephyrGpioBackend(k_work_q& work_queue, const ZephyrGpioConfig& config)
    : m_state(std::make_unique<State>(work_queue, config)) {}

ZephyrGpioBackend::~ZephyrGpioBackend() {
    if (!m_state) return;
    m_state->assert_application_thread();
    if (m_state->shutdown() != IoStatus::completed) {
        // Returning would leave gpio_callback registered in freed storage.
        std::terminate();
    }
}

IoStatus ZephyrGpioBackend::shutdown() noexcept {
    if (!m_state) return IoStatus::completed;
    m_state->assert_application_thread();
    return m_state->shutdown();
}

std::span<const DeviceOperation> ZephyrGpioBackend::operations() const noexcept {
    m_state->assert_application_thread();
    return m_state->operation_list();
}

IoStatus ZephyrGpioBackend::start_operation(
    const BackendOperation& operation,
    std::weak_ptr<PeripheralCompletionSink> completions) {
    m_state->assert_application_thread();
    return m_state->start(operation, std::move(completions));
}

IoStatus ZephyrGpioBackend::cancel_operation(const BackendOperation& operation) {
    m_state->assert_application_thread();
    return m_state->cancel(operation);
}

void ZephyrGpioBackend::release_application(std::uint64_t owner_generation) {
    m_state->assert_application_thread();
    m_state->release(owner_generation);
}

void ZephyrGpioBackend::poll_on_application_thread() {
    m_state->assert_application_thread();
    m_state->poll();
}

std::shared_ptr<ZephyrGpioBackend> register_zephyr_gpio(
    PeripheralRegistry& registry, k_work_q& work_queue, const ZephyrGpioConfig& config) {
    std::shared_ptr<ZephyrGpioBackend> backend(new ZephyrGpioBackend(work_queue, config));
    registry.register_endpoint(
        EndpointDescriptor{config.logical_name, gpio_version, false}, config.access, backend,
        [](PeripheralBackend& backend) {
            static_cast<ZephyrGpioBackend&>(backend).poll_on_application_thread();
        });
    return backend;
}

} // namespace mb::zephyr
