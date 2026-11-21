#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace mb {

// Numeric identifiers are part of the application ABI. Append, do not renumber.
enum class Capability : std::uint16_t {
    none = 0,
    gpio = 1,
    adc = 2,
    pwm = 3,
    pulse_capture = 4,
    pulse_output = 5,
    spi_controller = 6,
    spi_peripheral = 7,
    i2c_controller = 8,
    i2c_peripheral = 9,
    uart = 10,
    rs485 = 11,
    i2s_transmit = 12,
    i2s_receive = 13,
    pdm_receive = 14,
    ble_central = 15,
    ble_peripheral = 16,
    event_link = 17,
    usb_device = 18,
    usb_host = 19,
    spi_multilane = 20,
    event_link_groups = 21,
    espressif_gpio_matrix = 22,
};

struct CapabilityVersion {
    Capability capability = Capability::none;
    std::uint16_t major = 0;
    std::uint16_t minor = 0;

    [[nodiscard]] friend bool operator==(const CapabilityVersion&, const CapabilityVersion&) = default;
};

// owner_generation prevents cross-application reuse. object_generation handles
// disconnect/close followed by allocation of the same slot. Zero is empty.
struct DeviceHandle {
    Capability capability = Capability::none;
    std::uint16_t slot = 0;
    std::uint32_t object_generation = 0;
    std::uint64_t owner_generation = 0;

    [[nodiscard]] bool is_empty() const {
        return capability == Capability::none && slot == 0 && object_generation == 0 && owner_generation == 0;
    }
    [[nodiscard]] friend bool operator==(const DeviceHandle&, const DeviceHandle&) = default;
};

using ByteBuffer = std::vector<std::uint8_t>;
// A driver that creates a mutable buffer transfers ownership before returning it.
// It must not mutate the bytes after sharing this pointer with an application.
using ByteBufferPtr = std::shared_ptr<const ByteBuffer>;

struct DeviceBudget {
    std::uint32_t maximum_open_handles = 8;
    std::uint32_t maximum_pending_operations = 8;
    std::uint32_t maximum_bytes_per_buffer = 4096;
    std::uint32_t maximum_retained_bytes = 65'536;
};

enum class IoStatus : std::uint8_t {
    completed = 0,
    invalid_argument = 1,
    permission_denied = 2,
    unsupported = 3,
    busy = 4,
    timeout = 5,
    device_lost = 6,
    io_failure = 7,
    memory_exhausted = 8,
    cancelled = 9,
};

struct EndpointDescriptor {
    std::string_view logical_name;
    CapabilityVersion version;
    bool modeled = false;
};

struct DeviceProfile {
    std::span<const EndpointDescriptor> endpoints;
    DeviceBudget budget;
};

using IoPayload = std::variant<std::monostate, std::int64_t, bool, ByteBufferPtr>;

} // namespace mb
