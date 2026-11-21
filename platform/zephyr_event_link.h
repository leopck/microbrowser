#pragma once

#include "microbrowser/peripherals.h"

#include <nrfx_gpiote.h>
#include <nrfx_timer.h>
#include <zephyr/drivers/gpio.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace mb::zephyr {

inline constexpr std::size_t event_link_max_pending_operations = 2;
inline constexpr std::size_t event_link_max_logical_name_bytes = 63;

// The resource pointers select board-owner instances created with
// NRFX_TIMER_INSTANCE() and NRFX_GPIOTE_INSTANCE(). They must outlive this backend.
// Raw addresses, channel numbers and arbitrary route strings are not selectable
// by an application. Source GPIO flags may specify active level and pull only.
struct ZephyrEventLinkConfig {
    std::string_view logical_name;
    gpio_dt_spec source = {};
    GpioEdge edge = GpioEdge::both;
    nrfx_gpiote_t* gpiote = nullptr;
    nrfx_timer_t* timer = nullptr;
    EndpointAccess access = EndpointAccess::application;
};

// One owner-selected GPIO input event connects to a timer COUNT task and, on the
// same PPI/DPPI connection, a CC0 capture task. CC0 therefore observes genuine
// hardware routes; it is not replaced by an OS timestamp or a timer IRQ count.
// The counter is 32 bits and accumulates uint32 deltas on UI polling. More than
// 2^32 transitions between polls cannot be distinguished. Stop pauses the
// timer without clearing its count; start resumes it. A fresh lease starts at
// zero. No timer capture timestamp,
// pulse duration, channel-group route, or software-only fallback is advertised.
//
// Construction, start_operation(), poll_on_application_thread(), shutdown(), and
// destruction must run on the application/UI thread. start_operation() only
// validates and queues bounded work. Polling performs one queued operation and
// delivers completion references on that same thread. There is no worker, timer
// IRQ, GPIOTE IRQ, or cross-thread C++ shared_ptr/weak_ptr access. The source pin
// must not be reassigned by another driver while the route is open. This backend
// can coexist with an already initialized GPIOTE instance and initializes an
// uninitialized instance on open. That shared instance remains initialized on
// close. Other GPIOTE users are not disabled, and unrelated channels remain
// available to other drivers.
//
// On nRF5340 Bsim, only application TIMER0 is accepted because its custom TIMER
// register-array stride differs from application MDK for nonzero instances.
class ZephyrEventLinkBackend final : public PeripheralBackend {
public:
    ~ZephyrEventLinkBackend() override;
    ZephyrEventLinkBackend(const ZephyrEventLinkBackend&) = delete;
    ZephyrEventLinkBackend& operator=(const ZephyrEventLinkBackend&) = delete;

    // Idempotent after successful teardown. Errors are not hidden: failed resource
    // release leaves enough state for the owner to retry shutdown().
    [[nodiscard]] IoStatus shutdown() noexcept;

    [[nodiscard]] std::span<const DeviceOperation> operations() const noexcept override;
    IoStatus start_operation(const BackendOperation& operation,
                             std::weak_ptr<PeripheralCompletionSink> completions) override;
    IoStatus cancel_operation(const BackendOperation& operation) override;
    void release_application(std::uint64_t owner_generation) override;
    void poll_on_application_thread();

private:
    friend std::shared_ptr<ZephyrEventLinkBackend> register_zephyr_event_link(
        PeripheralRegistry& registry, const ZephyrEventLinkConfig& config);
    explicit ZephyrEventLinkBackend(const ZephyrEventLinkConfig& config);

    struct State;
    std::unique_ptr<State> m_state;
};

// Throws std::invalid_argument for a missing/unsupported owner resource,
// already initialized timer, used source pin, invalid edge policy, or duplicate
// endpoint. Timer and source-channel allocation failures occur on open instead.
[[nodiscard]] std::shared_ptr<ZephyrEventLinkBackend> register_zephyr_event_link(
    PeripheralRegistry& registry, const ZephyrEventLinkConfig& config);

} // namespace mb::zephyr
