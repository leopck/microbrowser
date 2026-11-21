#include "platform/zephyr_event_link.h"

#include <helpers/nrfx_gppi.h>
#include <zephyr/kernel.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <stdexcept>
#include <utility>

namespace mb::zephyr {
namespace {



constexpr CapabilityVersion event_link_version{Capability::event_link, 1, 0};
constexpr std::uint32_t timer_frequency_hz = 1'000'000;
constexpr std::array<DeviceOperation, 6> event_link_operations{
    DeviceOperation::open,
    DeviceOperation::start,
    DeviceOperation::stop,
    DeviceOperation::read,
    DeviceOperation::close,
    DeviceOperation::cancel,
};

[[nodiscard]] IoStatus map_nrfx_error(int error) {
    switch (error) {
    case 0: return IoStatus::completed;
    case -ENOMEM: return IoStatus::memory_exhausted;
    case -EBUSY:
    case -EALREADY: return IoStatus::busy;
    case -ENODEV:
    case -ENOTSUP: return IoStatus::unsupported;
    case -EACCES:
    case -EPERM: return IoStatus::permission_denied;
    default: return IoStatus::io_failure;
    }
}

[[nodiscard]] nrfx_gpiote_trigger_t source_trigger(const gpio_dt_spec& source,
                                                    GpioEdge edge) noexcept {
    const bool active_low = (source.dt_flags & GPIO_ACTIVE_LOW) != 0;
    if (edge == GpioEdge::both) return NRFX_GPIOTE_TRIGGER_TOGGLE;
    if (edge == GpioEdge::rising) {
        return active_low ? NRFX_GPIOTE_TRIGGER_HITOLO : NRFX_GPIOTE_TRIGGER_LOTOHI;
    }
    return active_low ? NRFX_GPIOTE_TRIGGER_LOTOHI : NRFX_GPIOTE_TRIGGER_HITOLO;
}

[[nodiscard]] nrfx_gpiote_pin_t source_gpiote_pin(const gpio_dt_spec& source) {
    constexpr gpio_pin_t pins_per_port = 32;
    if (source.pin >= pins_per_port) {
        throw std::invalid_argument("event-link source pin index is outside its GPIO port");
    }
    if (source.port == DEVICE_DT_GET(DT_NODELABEL(gpio0))) {
        return static_cast<nrfx_gpiote_pin_t>(source.pin);
    }
    if (source.port == DEVICE_DT_GET(DT_NODELABEL(gpio1))) {
        return static_cast<nrfx_gpiote_pin_t>(NRF_GPIO_PIN_MAP(1, source.pin));
    }
    throw std::invalid_argument("event-link source port must be GPIO0 or GPIO1");
}

struct PendingUiOperation {
    BackendOperation operation{};
    std::weak_ptr<PeripheralCompletionSink> completions;
    bool active = false;
    bool cancelled = false;
};

} // namespace

struct ZephyrEventLinkBackend::State final {
    explicit State(const ZephyrEventLinkConfig& endpoint) : config(endpoint) {
        if (config.logical_name.empty() ||
            config.logical_name.size() > event_link_max_logical_name_bytes ||
            config.gpiote == nullptr || config.timer == nullptr ||
            !gpio_is_ready_dt(&config.source)) {
            throw std::invalid_argument("event-link owner configuration is unavailable");
        }
        const gpio_flags_t supported_flags = GPIO_ACTIVE_LOW | GPIO_PULL_UP | GPIO_PULL_DOWN;
        if ((config.source.dt_flags & ~supported_flags) != 0 ||
            (config.edge != GpioEdge::both && config.edge != GpioEdge::rising &&
             config.edge != GpioEdge::falling)) {
            throw std::invalid_argument("event-link source flags or edge policy are unsupported");
        }
        if (nrfx_timer_init_check(config.timer)) {
            throw std::invalid_argument("event-link timer is already owned");
        }
#if defined(CONFIG_BOARD_NRF5340BSIM_NRF5340_CPUAPP)
        // Bsim's custom TIMER register stride disagrees with application MDK for nonzero instances.
        if (config.timer->p_reg != NRF_TIMER0) {
            throw std::invalid_argument("nRF5340 Bsim event-link requires application TIMER0");
        }
#endif

        std::uint8_t existing_channel = 0;
        if (nrfx_gpiote_channel_get(config.gpiote, source_pin, &existing_channel) == 0) {
            throw std::invalid_argument("event-link source pin already uses a GPIOTE event channel");
        }
    }

    void assert_application_thread() const {
        __ASSERT(!k_is_in_isr() && k_current_get() == ui_thread,
                 "event-link calls must run on the constructing UI thread");
        (void)ui_thread;
    }

    void snapshot_count() {
        if (!timer_initialized) return;
        const std::uint32_t captured = nrfx_timer_capture(timer, NRF_TIMER_CC_CHANNEL0);
        total_events += std::uint32_t(captured - last_capture);
        last_capture = captured;
    }

    IoStatus release_resources() noexcept {
        enabled = false;
        if (route_allocated) {
            nrfx_gpiote_trigger_disable(gpiote, source_pin);
            nrfx_gppi_conn_disable(link);
            nrfx_gppi_ep_disable(capture_address);
            nrfx_gppi_ep_clear(capture_address);
            nrfx_gppi_conn_free(event_address, count_address, link);
            route_allocated = false;
        }
        if (timer_initialized) {
            nrfx_timer_disable(timer);
            nrfx_timer_uninit(timer);
            timer_initialized = false;
        }
        IoStatus status = IoStatus::completed;
        if (pin_initialized) {
            nrfx_gpiote_trigger_disable(gpiote, source_pin);
            const int error = nrfx_gpiote_pin_uninit(gpiote, source_pin);
            if (error == 0) {
                pin_initialized = false;
            } else {
                status = map_nrfx_error(error);
            }
        }
        if (channel_allocated) {
            const int error = nrfx_gpiote_channel_free(gpiote, channel);
            if (error == 0) {
                channel_allocated = false;
            } else {
                status = map_nrfx_error(error);
            }
        }
        if (pin_initialized || channel_allocated) return status;
        owner_generation = 0;
        total_events = 0;
        last_capture = 0;
        return status;
    }

    IoStatus acquire_resources() {
        nrfx_timer_config_t timer_config = NRFX_TIMER_DEFAULT_CONFIG(timer_frequency_hz);
        timer_config.mode = NRF_TIMER_MODE_COUNTER;
        timer_config.bit_width = NRF_TIMER_BIT_WIDTH_32;
        if (!nrfx_gpiote_init_check(gpiote)) {
            const int gpiote_error = nrfx_gpiote_init(gpiote, NRFX_GPIOTE_DEFAULT_CONFIG_IRQ_PRIORITY);
            if (gpiote_error != 0 && gpiote_error != -EALREADY) {
                return map_nrfx_error(gpiote_error);
            }
        }

        int error = nrfx_timer_init(timer, &timer_config, nullptr);
        if (error != 0) return map_nrfx_error(error);
        timer_initialized = true;

        error = nrfx_gpiote_channel_alloc(gpiote, &channel);
        if (error != 0) {
            release_resources();
            return map_nrfx_error(error);
        }
        channel_allocated = true;
        const nrf_gpio_pin_pull_t pull = (config.source.dt_flags & GPIO_PULL_UP) != 0
            ? NRF_GPIO_PIN_PULLUP
            : ((config.source.dt_flags & GPIO_PULL_DOWN) != 0
                ? NRF_GPIO_PIN_PULLDOWN : NRF_GPIO_PIN_NOPULL);
        const nrfx_gpiote_trigger_config_t trigger{source_trigger(config.source, config.edge), &channel};
        const nrfx_gpiote_input_pin_config_t input{&pull, &trigger, nullptr};
        error = nrfx_gpiote_input_configure(gpiote, source_pin, &input);
        if (error != 0) {
            release_resources();
            return map_nrfx_error(error);
        }
        pin_initialized = true;
        nrfx_gpiote_trigger_disable(gpiote, source_pin);
        event = nrfx_gpiote_in_event_get(gpiote, source_pin);
        event_address = nrfx_gpiote_in_event_address_get(gpiote, source_pin);
        count_address = nrfx_timer_task_address_get(timer, NRF_TIMER_TASK_COUNT);
        capture_address = nrfx_timer_capture_task_address_get(timer, 0);
        error = nrfx_gppi_conn_alloc(event_address, count_address, &link);
        if (error != 0) {
            release_resources();
            return map_nrfx_error(error);
        }
        route_allocated = true;
        error = nrfx_gppi_ep_attach(capture_address, link);
        if (error != 0) {
            release_resources();
            return map_nrfx_error(error);
        }
        nrfx_timer_disable(timer);
        // Clear CC0 explicitly: TASK_CLEAR does not reset the capture register.
        nrfx_timer_compare(timer, NRF_TIMER_CC_CHANNEL0, 0, false);
        nrfx_timer_clear(timer);
        last_capture = nrfx_timer_capture_get(timer, NRF_TIMER_CC_CHANNEL0);
        total_events = 0;
        return IoStatus::completed;
    }

    IoStatus start_route() {
        if (enabled) return IoStatus::invalid_argument;
        snapshot_count();
        nrf_gpiote_event_clear(gpiote->p_reg, event);
        if (nrfx_timer_is_enabled(timer)) {
            nrfx_timer_resume(timer);
        } else {
            nrfx_timer_enable(timer);
        }
        // The route is enabled before the source, so no source event races setup.
        nrfx_gppi_conn_enable(link);
        nrfx_gpiote_trigger_enable(gpiote, source_pin, false);
        enabled = true;
        return IoStatus::completed;
    }


    void stop_route() noexcept {
        if (!enabled) return;
        enabled = false;
        nrfx_gpiote_trigger_disable(gpiote, source_pin);
        nrfx_gppi_conn_disable(link);
        snapshot_count();
        nrfx_timer_pause(timer);
    }


    IoStatus start(const BackendOperation& operation,
                   std::weak_ptr<PeripheralCompletionSink> completions) {
        if (shutting_down) return IoStatus::device_lost;
        const bool valid_request =
            (operation.id == DeviceOperation::open && std::holds_alternative<OpenRequest>(operation.request)) ||
            (operation.id == DeviceOperation::start && std::holds_alternative<StartRequest>(operation.request)) ||
            (operation.id == DeviceOperation::stop && std::holds_alternative<StopRequest>(operation.request)) ||
            (operation.id == DeviceOperation::read && std::holds_alternative<EventLinkReadRequest>(operation.request)) ||
            (operation.id == DeviceOperation::close && std::holds_alternative<CloseRequest>(operation.request));
        if (!valid_request) return IoStatus::invalid_argument;
        if (operation.id == DeviceOperation::open) {
            if (owner_generation != 0) return IoStatus::busy;
        } else if (owner_generation == 0 || operation.owner_generation != owner_generation) {
            return IoStatus::invalid_argument;
        }
        auto free = std::find_if(pending.begin(), pending.end(), [](const PendingUiOperation& entry) {
            return !entry.active;
        });
        if (free == pending.end()) return IoStatus::busy;
        free->operation = operation;
        free->completions = std::move(completions);
        free->cancelled = false;
        free->active = true;
        return IoStatus::completed;
    }

    IoStatus execute(const BackendOperation& operation, DeviceResponse& response) {
        switch (operation.id) {
        case DeviceOperation::open: {
            owner_generation = operation.owner_generation;
            const IoStatus status = acquire_resources();
            if (status != IoStatus::completed) owner_generation = 0;
            response = OpenResponse{};
            return status;
        }
        case DeviceOperation::start: {
            if (!route_allocated || !timer_initialized) return IoStatus::invalid_argument;
            const IoStatus status = start_route();
            response = EmptyResponse{};
            return status;
        }
        case DeviceOperation::stop:
            stop_route();
            response = EmptyResponse{};
            return IoStatus::completed;
        case DeviceOperation::read:
            snapshot_count();
            response = EventLinkReadResponse{
                enabled, enabled && route_allocated, static_cast<std::int64_t>(total_events)};
            return IoStatus::completed;
        case DeviceOperation::close:
            response = EmptyResponse{};
            return release_resources();
        default:
            return IoStatus::unsupported;
        }
    }

    IoStatus cancel(const BackendOperation& operation) {
        for (PendingUiOperation& entry : pending) {
            if (entry.active && entry.operation.owner_generation == operation.owner_generation &&
                entry.operation.operation == operation.operation) {
                entry.cancelled = true;
                return IoStatus::completed;
            }
        }
        return IoStatus::invalid_argument;
    }

    void deliver(std::size_t index) {
        PendingUiOperation work = std::move(pending[index]);
        pending[index] = {};
        DeviceResponse response = EmptyResponse{};
        const IoStatus status = work.cancelled ? IoStatus::cancelled
            : execute(work.operation, response);
        if (const auto completion = work.completions.lock()) {
            completion->complete(work.operation, status, std::move(response));
        }
    }

    void poll() {
        snapshot_count();
        for (std::size_t index = 0; index < pending.size(); ++index) {
            if (pending[index].active) {
                deliver(index);
                return;
            }
        }
    }

    IoStatus shutdown() noexcept {
        if (shutting_down) {
            if (pin_initialized || channel_allocated || enabled) return release_resources();
            return IoStatus::completed;
        }
        shutting_down = true;
        owner_generation = 0;
        return release_resources();
    }

    void release(std::uint64_t generation) {
        for (PendingUiOperation& entry : pending) {
            if (entry.active && entry.operation.owner_generation == generation) {
                entry.cancelled = true;
            }
        }
        if (owner_generation == generation) {
            stop_route();
            (void)release_resources();
        }
    }

    const ZephyrEventLinkConfig config;
    nrfx_gpiote_t* const gpiote = config.gpiote;
    nrfx_timer_t* const timer = config.timer;
    const k_thread* const ui_thread = k_current_get();
    std::array<PendingUiOperation, event_link_max_pending_operations> pending{};
    const nrfx_gpiote_pin_t source_pin = source_gpiote_pin(config.source);
    nrfx_gppi_handle_t link{};
    nrf_gpiote_event_t event{};
    std::uint32_t event_address = 0;
    std::uint32_t count_address = 0;
    std::uint32_t capture_address = 0;
    std::uint64_t owner_generation = 0;
    std::uint64_t total_events = 0;
    std::uint32_t last_capture = 0;
    std::uint8_t channel = 0;
    bool enabled = false;
    bool timer_initialized = false;
    bool pin_initialized = false;
    bool channel_allocated = false;
    bool route_allocated = false;
    bool shutting_down = false;
};

ZephyrEventLinkBackend::ZephyrEventLinkBackend(const ZephyrEventLinkConfig& config)
    : m_state(std::make_unique<State>(config)) {}

ZephyrEventLinkBackend::~ZephyrEventLinkBackend() {
    if (!m_state) return;
    m_state->assert_application_thread();
    if (m_state->shutdown() != IoStatus::completed) std::terminate();
}

IoStatus ZephyrEventLinkBackend::shutdown() noexcept {
    if (!m_state) return IoStatus::completed;
    m_state->assert_application_thread();
    return m_state->shutdown();
}

std::span<const DeviceOperation> ZephyrEventLinkBackend::operations() const noexcept {
    m_state->assert_application_thread();
    return event_link_operations;
}

IoStatus ZephyrEventLinkBackend::start_operation(
    const BackendOperation& operation,
    std::weak_ptr<PeripheralCompletionSink> completions) {
    m_state->assert_application_thread();
    return m_state->start(operation, std::move(completions));
}

IoStatus ZephyrEventLinkBackend::cancel_operation(const BackendOperation& operation) {
    m_state->assert_application_thread();
    return m_state->cancel(operation);
}

void ZephyrEventLinkBackend::release_application(std::uint64_t owner_generation) {
    m_state->assert_application_thread();
    m_state->release(owner_generation);
}

void ZephyrEventLinkBackend::poll_on_application_thread() {
    m_state->assert_application_thread();
    m_state->poll();
}

std::shared_ptr<ZephyrEventLinkBackend> register_zephyr_event_link(
    PeripheralRegistry& registry, const ZephyrEventLinkConfig& config) {
    std::shared_ptr<ZephyrEventLinkBackend> backend(new ZephyrEventLinkBackend(config));
    registry.register_endpoint(
        EndpointDescriptor{config.logical_name, event_link_version, false}, config.access, backend,
        [](PeripheralBackend& backend) {
            static_cast<ZephyrEventLinkBackend&>(backend).poll_on_application_thread();
        });
    return backend;
}

} // namespace mb::zephyr
