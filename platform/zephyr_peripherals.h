#pragma once

#include "microbrowser/peripherals.h"

#include <zephyr/drivers/gpio.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

struct k_work_q;

namespace mb::zephyr {

inline constexpr std::size_t gpio_max_open_handles = 8;
inline constexpr std::size_t gpio_max_pending_operations = 8;
inline constexpr std::size_t gpio_max_notification_samples = 8;
inline constexpr std::size_t gpio_max_bytes_per_buffer = 4096;
inline constexpr std::size_t gpio_max_logical_name_bytes = 63;

enum class GpioDirectionPermission : std::uint8_t {
    input_only,
    output_only,
    input_and_output,
};

// The gpio field, its controller, and its devicetree flags are board-owner
// inputs. applications never supply a controller or pin number. operations
// must contain open and close; supported optional entries are configure, read,
// write, subscribe, unsubscribe, and cancel.
struct ZephyrGpioConfig {
    std::string_view logical_name;
    gpio_dt_spec gpio = {};
    GpioDirectionPermission direction = GpioDirectionPermission::input_only;
    std::span<const DeviceOperation> operations;
    EndpointAccess access = EndpointAccess::application;
};

// One backend owns one gpio_dt_spec and at most one physical subscription.
// Each DeviceSession still enforces its own eight-handle budget; open itself
// allocates no backend state. Per backend, operation storage is eight entries
// and ISR notification storage is eight {level, timestamp, lease} POD records.
// UI-owned entries retain BackendOperation and weak completion references until
// application-thread result delivery or teardown. Worker and ISR paths access
// only trivially copyable tickets, scalar GPIO results, subscription metadata,
// and ring indexes protected by k_spinlock; they never access C++ request,
// response, shared_ptr, weak_ptr, variant, string, or completion-sink objects.
// A full ISR ring discards its oldest sample. Application-thread polling calls
// complete()/publish() for current work; a publish rejected with busy or
// memory_exhausted drops that sample. These are coalesced level notifications,
// not lossless edge capture, pulse measurement, or a substitute for event_link /
// pulse_capture. Timestamp resolution is CONFIG_SYS_CLOCK_TICKS_PER_SEC.
//
// All GPIO pin configuration, reads, writes, and interrupt setup run on the
// supplied k_work_q. Its thread must be started before construction and remain
// running and unplugged until shutdown. One work invocation performs at most
// one operation and any required physical subscription stop. Browser UI
// polling reconstructs DeviceResponse values, delivers completions, and drains
// at most eight notification samples per call. Nordic on-chip GPIO reads are
// register accesses and are suitable for this ISR path. Do not enable subscribe
// for a controller whose callbacks block or whose gpio_pin_get_raw() can sleep
// (for example, an I2C GPIO expander).
// Build with CONFIG_GPIO=y, CONFIG_CPP=y, CONFIG_STD_CPP20=y,
// CONFIG_REQUIRES_FULL_LIBCPP=y, CONFIG_GLIBCXX_LIBCPP=y, and
// CONFIG_CPP_EXCEPTIONS=y. Subscriptions require the selected GPIO driver's
// interrupt support (CONFIG_GPIO_NRFX_INTERRUPT on Nordic) and an available
// GPIOTE channel where that driver requires one. Pin configuration uses
// gpio_pin_configure_dt(), reads use gpio_pin_get_dt() (plus gpio_pin_get_raw()
// in the ISR), writes use gpio_pin_set_dt(), and subscriptions use
// gpio_init_callback(), gpio_add_callback_dt(), GPIO_INT_EDGE_* with
// gpio_pin_interrupt_configure_dt(), then gpio_remove_callback_dt() after
// GPIO_INT_DISABLE. ISR-to-thread handoff is k_work_submit_to_queue().
// DPPI/PPI, audio, event_link, and precise pulse timing are intentionally
// absent. The embedding owner supplies the stack and priority and sizes the
// full C++ runtime heap for its registry and application count.
//
// GPIO logical reads, writes, initial output values, and rising/falling edge
// selection honor GPIO_ACTIVE_LOW. A configure request's pull replaces only
// the pull bits in dt_flags; active level, drive mode, wake, and SoC flags are
// preserved. Reconfiguring a pin while it has a subscription returns busy.
// Close, unsubscribe, and application release suppress notifications at
// accepted submission and stop the physical subscription on the worker. Closing
// does not force a pin safe state because no board-wide safe-state policy is
// inferred here.
class ZephyrGpioBackend final : public PeripheralBackend {
public:
    // Construct, poll, shut down, and destroy on the application/UI thread that
    // constructed the backend. Other public calls have the same confinement;
    // misuse from another thread or an ISR raises a fatal Zephyr assertion.
    // shutdown() and destruction may wait for an executing k_work item, so
    // never call them from the work queue or an ISR. Keep the returned
    // shared_ptr alive beyond the registry so final destruction is
    // owner-controlled. shutdown() is idempotent; an error leaves the object
    // alive so the owner can retry without leaving a registered gpio_callback
    // pointing into freed storage.
    ~ZephyrGpioBackend() override;
    ZephyrGpioBackend(const ZephyrGpioBackend&) = delete;
    ZephyrGpioBackend& operator=(const ZephyrGpioBackend&) = delete;

    [[nodiscard]] IoStatus shutdown() noexcept;

    [[nodiscard]] std::span<const DeviceOperation> operations() const noexcept override;
    IoStatus start_operation(const BackendOperation& operation,
                             std::weak_ptr<PeripheralCompletionSink> completions) override;
    IoStatus cancel_operation(const BackendOperation& operation) override;
    void release_application(std::uint64_t owner_generation) override;
    // Reconstructs and delivers pending results on the application/UI thread.
    // DeviceSession invokes this through the registry delivery callback before
    // retrieving one result. No completion sink is called by the worker or ISR.
    void poll_on_application_thread();

private:
    friend std::shared_ptr<ZephyrGpioBackend> register_zephyr_gpio(
        PeripheralRegistry& registry, k_work_q& work_queue, const ZephyrGpioConfig& config);

    ZephyrGpioBackend(k_work_q& work_queue, const ZephyrGpioConfig& config);

    struct State;
    std::unique_ptr<State> m_state;
};

// Registers exactly one real, non-modeled GPIO endpoint and returns its owner
// lifetime handle. Construction throws std::invalid_argument for a null or
// unready controller, invalid policy, duplicate/unknown operations, an absent
// open/close operation, an over-limit name, or an unstarted work queue. An
// input-only endpoint cannot advertise write; an output-only endpoint cannot
// advertise subscribe; unsubscribe without subscribe is also rejected.
// No disabled DT node creates an endpoint or a successful open.
[[nodiscard]] std::shared_ptr<ZephyrGpioBackend> register_zephyr_gpio(
    PeripheralRegistry& registry, k_work_q& work_queue, const ZephyrGpioConfig& config);

} // namespace mb::zephyr
