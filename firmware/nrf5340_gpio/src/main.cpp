#include "microbrowser/peripherals.h"
#include "platform/zephyr_peripherals.h"
#include "platform/zephyr_event_link.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#if defined(MB_GPIO_EXPECT_SIMULATED_EDGES)
#include "nsi_main.h"
#endif

#include <array>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace mb {
namespace {

constexpr std::string_view button_name = "user_button";
constexpr mb::CapabilityVersion button_version{
    mb::Capability::gpio, mb::peripheral_abi_major, 0};
constexpr std::array<mb::DeviceOperation, 7> button_operations{
    mb::DeviceOperation::open, mb::DeviceOperation::configure,
    mb::DeviceOperation::read, mb::DeviceOperation::subscribe,
    mb::DeviceOperation::unsubscribe, mb::DeviceOperation::cancel,
    mb::DeviceOperation::close,
};
constexpr std::array<mb::CapabilityVersion, 1> capabilities{button_version};
constexpr std::array<std::string_view, 1> allowed_endpoints{button_name};
constexpr std::int64_t completion_timeout_ms = 1000;

struct ButtonInterface {
    ButtonInterface()
        : backend(zephyr::register_zephyr_gpio(
              registry, k_sys_work_q,
              {button_name, GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios),
               mb::zephyr::GpioDirectionPermission::input_only,
               std::span<const mb::DeviceOperation>(button_operations)})),
          session(registry.create_session({capabilities, allowed_endpoints})) {}

    mb::PeripheralRegistry registry;
    std::shared_ptr<zephyr::ZephyrGpioBackend> backend;
    std::unique_ptr<DeviceSession> session;
};

std::optional<DeviceResult> complete_after(mb::DeviceSession& session,
                                           mb::SubmissionResult submission,
                                           mb::DeviceOperation expected) {
    if (submission.status != IoStatus::completed || submission.operation.is_empty()) {
        printk("GPIO operation %u rejected: status %u\n",
               static_cast<unsigned>(expected), static_cast<unsigned>(submission.status));
        return std::nullopt;
    }

    const std::int64_t deadline = k_uptime_get() + completion_timeout_ms;
    while (k_uptime_get() < deadline) {
        auto result = session.poll();
        if (result && result->kind == DeviceResultKind::completion &&
            result->operation == submission.operation && result->id == expected) {
            if (result->status != IoStatus::completed) {
                printk("GPIO operation %u failed: status %u\n",
                       static_cast<unsigned>(expected), static_cast<unsigned>(result->status));
                return std::nullopt;
            }
            return result;
        }
        k_msleep(1);
    }
    printk("GPIO operation %u timed out\n", static_cast<unsigned>(expected));
    return std::nullopt;
}

bool open_device(mb::DeviceSession& session, std::string_view endpoint,
                  const CapabilityVersion& version, DeviceHandle& device) {
    auto result = complete_after(session, session.open(endpoint, mb::OpenRequest{version}),
                                 DeviceOperation::open);
    if (!result) return false;
    const auto* response = std::get_if<OpenResponse>(&result->response);
    if (response == nullptr || response->device.is_empty() || response->modeled ||
        response->version != version) {
        printk("peripheral open returned an invalid response\n");
        return false;
    }
    device = response->device;
    return true;
}

bool configure_button(mb::DeviceSession& session, const DeviceHandle& device) {
    auto result = complete_after(
        session, session.execute(device, DeviceOperation::configure,
                                 GpioConfigureRequest{GpioDirection::input, GpioPull::up, false}),
        DeviceOperation::configure);
    return result && std::holds_alternative<EmptyResponse>(result->response);
}

bool read_button(mb::DeviceSession& session, const DeviceHandle& device) {
    auto result = complete_after(
        session, session.execute(device, DeviceOperation::read, GpioReadRequest{}),
        DeviceOperation::read);
    if (!result) return false;
    const auto* response = std::get_if<GpioReadResponse>(&result->response);
    if (response == nullptr) return false;
    printk("GPIO button logical level=%u, timestamp_ns=%lld\n",
           response->high ? 1U : 0U, static_cast<long long>(response->timestamp_ns));
    return true;
}

bool unsubscribe_button(mb::DeviceSession& session, const DeviceHandle& device,
                        const DeviceHandle& subscription) {
    auto result = complete_after(
        session, session.execute(device, DeviceOperation::unsubscribe,
                                 UnsubscribeRequest{subscription}),
        DeviceOperation::unsubscribe);
    return result && std::holds_alternative<EmptyResponse>(result->response);
}

#ifdef MB_GPIO_EXPECT_SIMULATED_EDGES
bool await_alternating_edges(DeviceSession& session, const DeviceHandle& subscription,
                             std::int64_t deadline_ms) {
    constexpr std::array<bool, 2> expected{true, false};
    std::size_t received = 0;
    while (k_uptime_get() < deadline_ms && received < expected.size()) {
        auto result = session.poll();
        if (!result) {
            k_msleep(1);
            continue;
        }
        if (result->kind != DeviceResultKind::notification ||
            result->subscription != subscription || result->status != IoStatus::completed) {
            return false;
        }
        const auto* sample = std::get_if<GpioReadResponse>(&result->response);
        if (sample == nullptr) return false;
        if (sample->high != expected[received]) {
            printk("GPIO Bsim unexpected edge: received_level=%u expected=%u at %lld ms\n",
                   sample->high ? 1U : 0U, expected[received] ? 1U : 0U,
                   static_cast<long long>(k_uptime_get()));
            return false;
        }
        ++received;
    }
    if (received != expected.size()) {
        printk("GPIO Bsim received %zu of %zu expected notifications before deadline\n",
               received, expected.size());
        return false;
    }
    printk("GPIO Bsim alternating-edge notifications verified\n");
    return true;
}

#if defined(MB_BSIM_EVENT_LINK_TEST)
bool wait_for_button_release(DeviceSession& session, const DeviceHandle& device,
                             std::int64_t deadline_ms) {
    while (k_uptime_get() < deadline_ms) {
        const auto result = complete_after(
            session, session.execute(device, DeviceOperation::read, GpioReadRequest{}),
            DeviceOperation::read);
        if (!result) return false;
        const auto* reading = std::get_if<GpioReadResponse>(&result->response);
        if (reading == nullptr) return false;
        if (!reading->high) return true;
        k_msleep(1);
    }
    return false;
}
#endif

bool expect_no_notifications_until(DeviceSession& session, std::int64_t deadline_ms) {
    while (k_uptime_get() < deadline_ms) {
        const auto result = session.poll();
        if (result) return false;
        k_msleep(1);
    }
    printk("GPIO Bsim subscription revocation verified\n");
    return true;
}
#endif

#if defined(MB_BSIM_EVENT_LINK_TEST)
bool rejects_unsupported_bsim_timer(const gpio_dt_spec& source,
                                    nrfx_gpiote_t& gpiote) {
    static nrfx_timer_t incompatible_timer = NRFX_TIMER_INSTANCE(NRF_TIMER1);
    PeripheralRegistry registry;
    try {
        const auto backend = zephyr::register_zephyr_event_link(
            registry, {"incompatible_timer", source, GpioEdge::both, &gpiote, &incompatible_timer});
        (void)backend;
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

bool event_link_state_matches(DeviceSession& session, const DeviceHandle& device,
                              bool expected_enabled, std::int64_t expected_count) {
    const auto result = complete_after(
        session, session.execute(device, DeviceOperation::read, EventLinkReadRequest{}),
        DeviceOperation::read);
    if (!result) return false;
    const auto* response = std::get_if<EventLinkReadResponse>(&result->response);
    return response != nullptr && response->enabled == expected_enabled &&
        response->hardware_autonomous == expected_enabled &&
        response->trigger_count == expected_count;
}


int exercise_event_link() {
    static nrfx_gpiote_t gpiote = NRFX_GPIOTE_INSTANCE(NRF_GPIOTE);
    static nrfx_timer_t timer = NRFX_TIMER_INSTANCE(NRF_TIMER0);

    constexpr std::string_view endpoint_name = "route_test";
    const gpio_dt_spec source = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios);
    if (!rejects_unsupported_bsim_timer(source, gpiote)) return 1;
    constexpr std::array<CapabilityVersion, 1> route_capability{{
        {Capability::event_link, peripheral_abi_major, 0}}};
    constexpr std::array<std::string_view, 1> allowed_route{endpoint_name};
    PeripheralRegistry registry;
    const auto backend = zephyr::register_zephyr_event_link(
        registry, {endpoint_name, source, GpioEdge::both, &gpiote, &timer});
    const auto session = registry.create_session({route_capability, allowed_route});
    DeviceHandle device;
    if (!open_device(*session, endpoint_name, route_capability[0], device)) return 1;

    const auto start = session->execute(device, DeviceOperation::start, StartRequest{});
    if (start.status != IoStatus::completed || session->cancel(start.operation) != IoStatus::completed) {
        return 1;
    }
    bool cancelled = false;
    const int64_t cancel_deadline = k_uptime_get() + completion_timeout_ms;
    while (k_uptime_get() < cancel_deadline) {
        const auto result = session->poll();
        if (result && result->operation == start.operation &&
            result->status == IoStatus::cancelled) {
            cancelled = true;
            break;
        }
        k_msleep(1);
    }
    if (!cancelled) return 1;
    const auto started = complete_after(
        *session, session->execute(device, DeviceOperation::start, StartRequest{}),
        DeviceOperation::start);
    if (!started) return 1;


    int64_t captured_count = 0;
    const int64_t deadline = k_uptime_get() + 22'000;
    while (k_uptime_get() < deadline) {
        const auto reading = complete_after(
            *session, session->execute(device, DeviceOperation::read, EventLinkReadRequest{}),
            DeviceOperation::read);
        if (!reading) return 1;
        const auto* response = std::get_if<EventLinkReadResponse>(&reading->response);
        if (response == nullptr || !response->enabled || !response->hardware_autonomous) return 1;
        captured_count = response->trigger_count;
        if (captured_count == 4) break;
        if (captured_count > 4) return 1;
        k_msleep(1);
    }
    if (captured_count != 4) {
        printk("event-link Bsim did not observe four hardware captures: %lld\n",
               static_cast<long long>(captured_count));
        return 1;
    }
    const auto stopped = complete_after(
        *session, session->execute(device, DeviceOperation::stop, StopRequest{}),
        DeviceOperation::stop);
    if (!stopped) return 1;
    const auto reading = complete_after(
        *session, session->execute(device, DeviceOperation::read, EventLinkReadRequest{}),
        DeviceOperation::read);
    if (!reading) return 1;
    const auto* stopped_reading = std::get_if<EventLinkReadResponse>(&reading->response);
    if (stopped_reading == nullptr || stopped_reading->enabled || stopped_reading->hardware_autonomous ||
        stopped_reading->trigger_count != 4) {
        return 1;
    }
    const auto resumed = complete_after(
        *session, session->execute(device, DeviceOperation::start, StartRequest{}),
        DeviceOperation::start);
    if (!resumed || !event_link_state_matches(*session, device, true, 4)) return 1;
    const auto stopped_again = complete_after(
        *session, session->execute(device, DeviceOperation::stop, StopRequest{}),
        DeviceOperation::stop);
    if (!stopped_again || !event_link_state_matches(*session, device, false, 4)) return 1;
    const auto closed = complete_after(
        *session, session->execute(device, DeviceOperation::close, CloseRequest{}),
        DeviceOperation::close);
    if (!closed) return 1;
    session->request_shutdown();
    if (backend->shutdown() != IoStatus::completed) return 1;
    printk("event-link Bsim COUNT/capture=4, stop/start count preserved, close completed\n");
    return 0;
}
#endif


int exercise_button() {
    ButtonInterface button;
    mb::DeviceHandle device;
    if (!open_device(*button.session, button_name, button_version, device) ||
        !configure_button(*button.session, device) ||
        !read_button(*button.session, device)) {
        return 1;
    }

#if defined(MB_BSIM_EVENT_LINK_TEST)
    if (!wait_for_button_release(*button.session, device, k_uptime_get() + 2'000)) {
        return 1;
    }
#endif

    auto subscribed = complete_after(
        *button.session,
        button.session->execute(device, DeviceOperation::subscribe, GpioSubscribeRequest{}),
        DeviceOperation::subscribe);
    if (!subscribed) return 1;
    const auto* response = std::get_if<SubscriptionResponse>(&subscribed->response);
    if (response == nullptr || response->subscription.is_empty()) return 1;

#ifdef MB_GPIO_EXPECT_SIMULATED_EDGES
    const std::int64_t notification_deadline = k_uptime_get() + 24'000;
    if (!await_alternating_edges(*button.session, response->subscription,
                                 notification_deadline)) {
        return 1;
    }
#endif

    if (!read_button(*button.session, device) ||
        !unsubscribe_button(*button.session, device, response->subscription)) {
        return 1;
    }

#ifdef MB_GPIO_EXPECT_SIMULATED_EDGES
    if (!expect_no_notifications_until(*button.session, notification_deadline)) return 1;
#endif

    const auto rejected = button.session->execute(
        device, DeviceOperation::write, GpioWriteRequest{false});
    if (rejected.status != IoStatus::unsupported || !rejected.operation.is_empty()) return 1;

    const auto closing = button.session->execute(
        device, DeviceOperation::close, CloseRequest{});
    if (closing.status != IoStatus::completed || closing.operation.is_empty()) return 1;
    button.session->request_shutdown();
    if (button.backend->shutdown() != IoStatus::completed) return 1;

    printk("GPIO SDK sequence passed for nRF5340 DK sw0 (P0.23)\n");
    return 0;
}

} // namespace
} // namespace mb

int main() {
    int result = 1;
    try {
#if defined(MB_BSIM_EVENT_LINK_TEST)
        result = mb::exercise_event_link();
#endif
        if (result == 0) result = mb::exercise_button();
    } catch (const std::exception& error) {
        printk("GPIO SDK setup failed: %s\n", error.what());
    }
#if defined(MB_GPIO_EXPECT_SIMULATED_EDGES)
    nsi_exit(result);
#endif
    return result;
}
