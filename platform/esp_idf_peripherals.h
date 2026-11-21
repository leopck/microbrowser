#pragma once

#include "microbrowser/peripherals.h"

#include "driver/gpio.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace mb {

inline constexpr CapabilityVersion esp_idf_gpio_version{
    Capability::gpio, peripheral_abi_major, 0};

enum class EspGpioOperations : std::uint8_t {
    none = 0,
    configure = 1U << 0,
    read = 1U << 1,
    write = 1U << 2,
    subscribe = 1U << 3,
};

[[nodiscard]] constexpr EspGpioOperations operator|(
    EspGpioOperations left, EspGpioOperations right) noexcept {
    return static_cast<EspGpioOperations>(
        static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
}

[[nodiscard]] constexpr bool esp_gpio_operation_allowed(
    EspGpioOperations allowed, EspGpioOperations operation) noexcept {
    return (static_cast<std::uint8_t>(allowed) &
            static_cast<std::uint8_t>(operation)) != 0;
}

inline constexpr EspGpioOperations esp_gpio_input_operations =
    EspGpioOperations::configure | EspGpioOperations::read |
    EspGpioOperations::subscribe;
inline constexpr EspGpioOperations esp_gpio_output_operations =
    EspGpioOperations::configure | EspGpioOperations::read |
    EspGpioOperations::write;

// Only these owner-declared aliases can reach gpio_config/gpio_set_level. The
// direction is fixed; in particular, an input alias can never become an output.
struct EspGpioEndpointConfig {
    std::string_view logical_name;
    gpio_num_t pin = GPIO_NUM_NC;
    GpioDirection direction = GpioDirection::input;
    EspGpioOperations operations = EspGpioOperations::none;
};

class EspIdfGpioBackend final : public PeripheralBackend {
public:
    explicit EspIdfGpioBackend(std::span<const EspGpioEndpointConfig> endpoints);
    ~EspIdfGpioBackend() override;

    EspIdfGpioBackend(const EspIdfGpioBackend&) = delete;
    EspIdfGpioBackend& operator=(const EspIdfGpioBackend&) = delete;
    EspIdfGpioBackend(EspIdfGpioBackend&&) = delete;
    EspIdfGpioBackend& operator=(EspIdfGpioBackend&&) = delete;

    [[nodiscard]] std::span<const DeviceOperation> operations() const noexcept override;
    IoStatus start_operation(
        const BackendOperation& operation,
        std::weak_ptr<PeripheralCompletionSink> completions) override;
    IoStatus cancel_operation(const BackendOperation& operation) override;
    void release_application(std::uint64_t owner_generation) override;

private:
    struct State;
    std::unique_ptr<State> m_state;
};

// Registers into an application-wide registry owned by the board. Declare the
// shared registry before this object, and this object before Browser. Edge
// notifications are bounded. Exact/high-rate event capture must use an allowed
// hardware event or pulse resource, not coalesced level notifications.
class EspIdfGpioRegistry final {
public:
    EspIdfGpioRegistry(PeripheralRegistry& registry,
                       std::span<const EspGpioEndpointConfig> endpoints);

    EspIdfGpioRegistry(const EspIdfGpioRegistry&) = delete;
    EspIdfGpioRegistry& operator=(const EspIdfGpioRegistry&) = delete;
    EspIdfGpioRegistry(EspIdfGpioRegistry&&) = delete;
    EspIdfGpioRegistry& operator=(EspIdfGpioRegistry&&) = delete;

    [[nodiscard]] PeripheralRegistry& registry() noexcept { return m_registry; }

private:
    PeripheralRegistry& m_registry;
    std::shared_ptr<EspIdfGpioBackend> m_gpio;
};

} // namespace mb
