#include "microbrowser/device_api.h"

#include "microbrowser/browser.h"

#include <array>
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace mb {
namespace {

constexpr DeviceBudget device_budget{};
constexpr std::size_t device_callback_arity = 3;

[[noreturn]] void device_error(std::string_view message) {
    throw ScriptError("device: " + std::string(message));
}

void require_arity(std::span<const Value> arguments, std::size_t expected) {
    if (arguments.size() != expected) {
        device_error("wrong argument count, expected " + std::to_string(expected) +
                     ", received " + std::to_string(arguments.size()));
    }
}

const Value& argument_at(std::span<const Value> arguments, std::size_t index) {
    if (index >= arguments.size()) device_error("missing argument");
    return arguments[index];
}

std::int64_t integer_from_number(float number) {
    if (!std::isfinite(number) || std::trunc(number) != number) {
        device_error("integer argument must be finite and exactly integral");
    }
    const double widened = static_cast<double>(number);
    if (widened < -9223372036854775808.0 || widened >= 9223372036854775808.0) {
        device_error("integer argument overflows int64");
    }
    return static_cast<std::int64_t>(number);
}

std::int64_t integer_from_text(std::string_view text) {
    if (text.empty()) device_error("integer text is empty");
    std::size_t position = text.front() == '-' ? 1 : 0;
    if (position == text.size()) device_error("integer text has no digits");
    for (; position < text.size(); ++position) {
        if (text[position] < '0' || text[position] > '9') {
            device_error("integer text must be optional-minus signed decimal without whitespace");
        }
    }
    std::int64_t result = 0;
    const auto converted = std::from_chars(text.data(), text.data() + text.size(), result, 10);
    if (converted.ec == std::errc::result_out_of_range) device_error("integer text overflows int64");
    if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size()) {
        device_error("invalid signed decimal integer");
    }
    return result;
}

std::int64_t integer_value(const Value& value, bool permit_text) {
    switch (value.type) {
    case Value::Type::integer: return value.integer;
    case Value::Type::number: return integer_from_number(value.number);
    case Value::Type::string:
        if (permit_text) return integer_from_text(value.string);
        break;
    default: break;
    }
    device_error(permit_text ? "expected int64, exactly-integral f32, or signed decimal text"
                             : "expected int64 or exactly-integral f32");
}


std::uint64_t unsigned_value(const Value& value, std::uint64_t maximum) {
    const std::int64_t integer = integer_value(value, false);
    if (integer < 0 || static_cast<std::uint64_t>(integer) > maximum) {
        device_error("unsigned integer argument is outside 0.." + std::to_string(maximum));
    }
    return static_cast<std::uint64_t>(integer);
}

template <typename T>
T unsigned_argument(std::span<const Value> arguments, std::size_t index) {
    static_assert(std::is_unsigned_v<T>);
    return static_cast<T>(unsigned_value(argument_at(arguments, index),
                                         std::numeric_limits<T>::max()));
}

bool bool_argument(std::span<const Value> arguments, std::size_t index) {
    const Value& value = argument_at(arguments, index);
    if (value.type != Value::Type::boolean) device_error("expected a boolean argument");
    return value.boolean;
}

const std::string& string_argument(std::span<const Value> arguments, std::size_t index) {
    const Value& value = argument_at(arguments, index);
    if (value.type != Value::Type::string) device_error("expected a string argument");
    return value.string;
}

DeviceHandle handle_argument(std::span<const Value> arguments, std::size_t index) {
    const Value& value = argument_at(arguments, index);
    if (value.type != Value::Type::handle || value.handle.is_empty()) {
        device_error("expected a nonempty opaque device handle");
    }
    return value.handle;
}

const ByteBufferPtr& bytes_argument(std::span<const Value> arguments, std::size_t index) {
    const Value& value = argument_at(arguments, index);
    if (value.type != Value::Type::bytes || !value.bytes) {
        device_error("expected an immutable byte buffer");
    }
    if (value.bytes->size() > device_budget.maximum_bytes_per_buffer) {
        device_error("byte buffer exceeds maximum_bytes_per_buffer");
    }
    return value.bytes;
}

Value integer_result(std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        device_error("result does not fit exact int64");
    }
    return Value::from_integer(static_cast<std::int64_t>(value));
}

Value count_result(std::size_t value) { return integer_result(static_cast<std::uint64_t>(value)); }

Capability capability_value(std::span<const Value> arguments, std::size_t index) {
    const auto raw = unsigned_argument<std::uint16_t>(arguments, index);
    if (raw < static_cast<std::uint16_t>(Capability::gpio) ||
        raw > static_cast<std::uint16_t>(Capability::espressif_gpio_matrix)) {
        device_error("unknown capability identifier");
    }
    return static_cast<Capability>(raw);
}

template <typename Enum>
Enum bounded_enum(std::span<const Value> arguments, std::size_t index, std::uint64_t maximum) {
    return static_cast<Enum>(unsigned_value(argument_at(arguments, index), maximum));
}

ByteBufferPtr require_duration_bytes(std::span<const Value> arguments, std::size_t index) {
    const auto& bytes = bytes_argument(arguments, index);
    if (bytes->empty() || bytes->size() % sizeof(std::uint64_t) != 0) {
        device_error("pulse durations must be nonempty little-endian u64 words");
    }
    return bytes;
}

std::shared_ptr<const NanosecondSequence> duration_sequence(const ByteBuffer& bytes) {
    NanosecondSequence durations;
    durations.reserve(bytes.size() / sizeof(std::uint64_t));
    for (std::size_t offset = 0; offset < bytes.size(); offset += sizeof(std::uint64_t)) {
        std::uint64_t value = 0;
        for (unsigned byte = 0; byte < sizeof(std::uint64_t); ++byte) {
            value |= static_cast<std::uint64_t>(bytes[offset + byte]) << (byte * 8U);
        }
        durations.push_back(value);
    }
    return std::make_shared<const NanosecondSequence>(std::move(durations));
}

BleUuid uuid_argument(std::span<const Value> arguments, std::size_t index, bool wildcard_allowed) {
    const auto& encoded = bytes_argument(arguments, index);
    if (encoded->empty() && wildcard_allowed) return {};
    if (encoded->size() != 2 && encoded->size() != 16) {
        device_error("BLE UUID must contain exactly 2 or 16 bytes");
    }
    BleUuid result;
    result.length = static_cast<std::uint8_t>(encoded->size());
    std::copy(encoded->begin(), encoded->end(), result.bytes.begin());
    return result;
}

BleAttribute attribute_arguments(std::span<const Value> arguments, std::size_t first,
                                 bool wildcard_allowed = false) {
    return BleAttribute{uuid_argument(arguments, first, wildcard_allowed),
                        uuid_argument(arguments, first + 1, wildcard_allowed)};
}

BleAddress address_argument(std::span<const Value> arguments, std::size_t address_index,
                            std::size_t random_index) {
    const auto& encoded = bytes_argument(arguments, address_index);
    if (encoded->size() != 6) device_error("BLE peer address must contain exactly 6 bytes");
    BleAddress result;
    std::copy(encoded->begin(), encoded->end(), result.bytes.begin());
    result.random = bool_argument(arguments, random_index);
    return result;
}

std::uint32_t receive_size(std::span<const Value> arguments, std::size_t index,
                           bool nonzero = false) {
    const auto count = unsigned_argument<std::uint32_t>(arguments, index);
    if ((nonzero && count == 0) || count > device_budget.maximum_bytes_per_buffer) {
        device_error("receive size is zero or exceeds maximum_bytes_per_buffer");
    }
    return count;
}

template <typename Entry>
std::uint32_t discovery_size(std::span<const Value> arguments, std::size_t index) {
    const auto count = unsigned_argument<std::uint32_t>(arguments, index);
    constexpr std::uint64_t maximum = device_budget.maximum_retained_bytes / sizeof(Entry);
    if (count == 0 || static_cast<std::uint64_t>(count) > maximum) {
        device_error("maximum_results must be nonzero and fit retained device storage");
    }
    return count;
}

std::uint32_t pulse_limit(std::span<const Value> arguments, std::size_t index) {
    const auto count = unsigned_argument<std::uint32_t>(arguments, index);
    constexpr std::uint64_t storage_limit = device_budget.maximum_retained_bytes / sizeof(PulseSample);
    constexpr std::uint64_t encoding_limit = device_budget.maximum_bytes_per_buffer / 24U;
    if (count == 0 || static_cast<std::uint64_t>(count) > storage_limit ||
        static_cast<std::uint64_t>(count) > encoding_limit) {
        device_error("maximum_pulses must be nonzero and fit retained and encoded pulse budgets");
    }
    return count;
}

std::uint32_t frame_limit(std::span<const Value> arguments, std::size_t index) {
    const auto count = unsigned_argument<std::uint32_t>(arguments, index);
    if (count == 0 || count > device_budget.maximum_bytes_per_buffer) {
        device_error("frame count must be nonzero and fit maximum_bytes_per_buffer");
    }
    return count;
}

PcmFormat pcm_format_arguments(std::span<const Value> arguments, std::size_t first) {
    PcmFormat format;
    format.frame_rate_hz = unsigned_argument<std::uint32_t>(arguments, first);
    format.channel_count = unsigned_argument<std::uint8_t>(arguments, first + 1);
    format.sample_format = bounded_enum<PcmSampleFormat>(arguments, first + 2, 3);
    return format;
}

std::uint16_t read_usb_u16(const ByteBuffer& bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 2) {
        device_error("USB binary record is truncated");
    }
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

void check_payload_budget(std::uint64_t total, std::uint64_t addition) {
    if (addition > device_budget.maximum_retained_bytes ||
        total > device_budget.maximum_retained_bytes - addition) {
        device_error("USB request exceeds maximum_retained_bytes");
    }
}

UsbDeviceConfigureRequest usb_configure_arguments(std::span<const Value> arguments) {
    require_arity(arguments, 2);
    UsbDeviceConfigureRequest request;
    request.device_descriptor = bytes_argument(arguments, 0);
    const auto& table = bytes_argument(arguments, 1);
    const std::uint16_t count = read_usb_u16(*table, 0);
    if (count > 8) device_error("USB interface_table allows at most eight interfaces");

    std::array<bool, 256> interface_numbers{};
    std::uint64_t retained = count * sizeof(UsbInterfaceDescriptor);
    check_payload_budget(retained, request.device_descriptor->size());
    retained += request.device_descriptor->size();
    std::size_t offset = 2;
    for (std::uint16_t index = 0; index < count; ++index) {
        if (offset > table->size() || table->size() - offset < 6) {
            device_error("USB interface_table entry is truncated");
        }
        const std::uint8_t number = (*table)[offset];
        if (interface_numbers[number]) device_error("USB interface_number is duplicated");
        interface_numbers[number] = true;
        const std::uint16_t descriptor_bytes = read_usb_u16(*table, offset + 4);
        if (descriptor_bytes > device_budget.maximum_bytes_per_buffer) {
            device_error("USB interface descriptors exceed maximum_bytes_per_buffer");
        }
        offset += 6;
        if (offset > table->size() || descriptor_bytes > table->size() - offset) {
            device_error("USB interface descriptor data is truncated");
        }
        check_payload_budget(retained, descriptor_bytes);
        retained += descriptor_bytes;
        offset += descriptor_bytes;
    }
    if (offset != table->size()) device_error("USB interface_table has trailing data");

    request.interfaces.reserve(count);
    offset = 2;
    for (std::uint16_t index = 0; index < count; ++index) {
        UsbInterfaceDescriptor descriptor;
        descriptor.interface_number = (*table)[offset];
        descriptor.interface_class = (*table)[offset + 1];
        descriptor.interface_subclass = (*table)[offset + 2];
        descriptor.interface_protocol = (*table)[offset + 3];
        const std::uint16_t descriptor_bytes = read_usb_u16(*table, offset + 4);
        offset += 6;
        descriptor.descriptors = make_immutable_bytes(
            std::span<const std::uint8_t>(table->data() + offset, descriptor_bytes));
        request.interfaces.push_back(std::move(descriptor));
        offset += descriptor_bytes;
    }
    return request;
}

UsbTransferRequest usb_transfer_arguments(std::span<const Value> arguments) {
    require_arity(arguments, 4);
    const auto& setup = bytes_argument(arguments, 0);
    const auto& transmit = bytes_argument(arguments, 1);
    if (setup->size() != 8) device_error("USB setup packet must contain exactly eight bytes");
    UsbTransferRequest request;
    request.setup.request_type = (*setup)[0];
    request.setup.request = (*setup)[1];
    request.setup.value = read_usb_u16(*setup, 2);
    request.setup.index = read_usb_u16(*setup, 4);
    const std::uint16_t declared_length = read_usb_u16(*setup, 6);
    request.bytes = transmit;
    request.receive_bytes = unsigned_argument<std::uint16_t>(arguments, 2);
    request.timeout_ms = unsigned_argument<std::uint32_t>(arguments, 3);
    if (request.receive_bytes > device_budget.maximum_bytes_per_buffer) {
        device_error("USB receive_bytes exceeds maximum_bytes_per_buffer");
    }
    const bool device_to_host = (request.setup.request_type & 0x80U) != 0;
    if (device_to_host) {
        if (request.receive_bytes != declared_length || !transmit->empty()) {
            device_error("USB device-to-host transfer requires receive_bytes=wLength and empty transmit");
        }
    } else if (transmit->size() != declared_length || request.receive_bytes != 0) {
        device_error("USB host-to-device transfer requires transmit.size=wLength and receive_bytes=0");
    }
    return request;
}

Value native_usb_setup(std::span<const Value> arguments) {
    require_arity(arguments, 5);
    ByteBuffer output(8, 0);
    output[0] = unsigned_argument<std::uint8_t>(arguments, 0);
    output[1] = unsigned_argument<std::uint8_t>(arguments, 1);
    const std::array<std::uint16_t, 3> fields{
        unsigned_argument<std::uint16_t>(arguments, 2),
        unsigned_argument<std::uint16_t>(arguments, 3),
        unsigned_argument<std::uint16_t>(arguments, 4)};
    for (std::size_t field = 0; field < fields.size(); ++field) {
        output[2 + field * 2] = static_cast<std::uint8_t>(fields[field]);
        output[3 + field * 2] = static_cast<std::uint8_t>(fields[field] >> 8U);
    }
    return Value::from_bytes(std::move(output));
}

DeviceRequest configure_request(Capability capability, std::span<const Value> arguments) {
    switch (capability) {
    case Capability::gpio:
        require_arity(arguments, 3);
        return GpioConfigureRequest{bounded_enum<GpioDirection>(arguments, 0, 1),
                                    bounded_enum<GpioPull>(arguments, 1, 2),
                                    bool_argument(arguments, 2)};
    case Capability::adc:
        require_arity(arguments, 2);
        return AdcConfigureRequest{unsigned_argument<std::uint64_t>(arguments, 0),
                                   unsigned_argument<std::uint32_t>(arguments, 1)};
    case Capability::pwm:
        require_arity(arguments, 2);
        return PwmConfigureRequest{unsigned_argument<std::uint64_t>(arguments, 0),
                                   unsigned_argument<std::uint64_t>(arguments, 1)};
    case Capability::spi_controller:
    case Capability::spi_peripheral:
    case Capability::spi_multilane: {
        require_arity(arguments, 4);
        const auto lanes = unsigned_argument<std::uint8_t>(arguments, 3);
        if (lanes != 1 && lanes != 2 && lanes != 4) device_error("SPI lanes must be 1, 2, or 4");
        return SpiConfigureRequest{unsigned_argument<std::uint32_t>(arguments, 0),
                                   bounded_enum<SpiMode>(arguments, 1, 3),
                                   bounded_enum<BitOrder>(arguments, 2, 1),
                                   static_cast<SpiLaneCount>(lanes)};
    }
    case Capability::i2c_controller:
    case Capability::i2c_peripheral:
        require_arity(arguments, 1);
        return I2cConfigureRequest{unsigned_argument<std::uint32_t>(arguments, 0)};
    case Capability::uart:
    case Capability::rs485:
        require_arity(arguments, 6);
        return UartConfigureRequest{unsigned_argument<std::uint32_t>(arguments, 0),
                                    unsigned_argument<std::uint8_t>(arguments, 1),
                                    unsigned_argument<std::uint8_t>(arguments, 2),
                                    bounded_enum<UartParity>(arguments, 3, 2),
                                    bounded_enum<UartFlowControl>(arguments, 4, 1),
                                    bounded_enum<Rs485Mode>(arguments, 5, 1)};
    case Capability::pulse_capture:
        require_arity(arguments, 2);
        return PulseCaptureConfigureRequest{unsigned_argument<std::uint64_t>(arguments, 0),
                                            pulse_limit(arguments, 1)};
    case Capability::pulse_output:
        require_arity(arguments, 2);
        return PulseOutputConfigureRequest{unsigned_argument<std::uint64_t>(arguments, 0),
                                           bounded_enum<PulseLevel>(arguments, 1, 1)};
    case Capability::i2s_transmit:
    case Capability::i2s_receive:
    case Capability::pdm_receive:
        require_arity(arguments, 4);
        return PcmConfigureRequest{pcm_format_arguments(arguments, 0), frame_limit(arguments, 3)};
    case Capability::usb_device:
        return usb_configure_arguments(arguments);
    default: device_error("capability does not support configure");
    }
}

DeviceRequest read_request(Capability capability, std::span<const Value> arguments) {
    switch (capability) {
    case Capability::gpio:
        require_arity(arguments, 0);
        return GpioReadRequest{};
    case Capability::adc:
        require_arity(arguments, 0);
        return AdcReadRequest{};
    case Capability::event_link:
        require_arity(arguments, 0);
        return EventLinkReadRequest{};
    case Capability::pulse_capture:
        require_arity(arguments, 1);
        return PulseCaptureReadRequest{pulse_limit(arguments, 0)};
    case Capability::spi_peripheral:
        require_arity(arguments, 1);
        return SpiPeripheralReadRequest{receive_size(arguments, 0, true)};
    case Capability::i2c_controller:
        require_arity(arguments, 3);
        return I2cReadRequest{unsigned_argument<std::uint16_t>(arguments, 0),
                              bool_argument(arguments, 1), receive_size(arguments, 2, true)};
    case Capability::i2c_peripheral:
        require_arity(arguments, 1);
        return I2cPeripheralReadRequest{receive_size(arguments, 0, true)};
    case Capability::uart:
    case Capability::rs485:
        require_arity(arguments, 1);
        return UartReadRequest{receive_size(arguments, 0, true)};
    case Capability::i2s_receive:
    case Capability::pdm_receive:
        require_arity(arguments, 1);
        return PcmReadRequest{frame_limit(arguments, 0)};
    case Capability::ble_central:
    case Capability::ble_peripheral:
        require_arity(arguments, 2);
        return BleReadRequest{attribute_arguments(arguments, 0)};
    case Capability::usb_device:
    case Capability::usb_host:
        require_arity(arguments, 3);
        return UsbReadRequest{unsigned_argument<std::uint8_t>(arguments, 0),
                              receive_size(arguments, 1, true),
                              unsigned_argument<std::uint32_t>(arguments, 2)};
    default: device_error("capability does not support read");
    }
}

DeviceRequest write_request(Capability capability, std::span<const Value> arguments) {
    switch (capability) {
    case Capability::gpio:
        require_arity(arguments, 1);
        return GpioWriteRequest{bool_argument(arguments, 0)};
    case Capability::pwm:
        require_arity(arguments, 1);
        return PwmWriteRequest{unsigned_argument<std::uint64_t>(arguments, 0)};
    case Capability::pulse_output: {
        require_arity(arguments, 1);
        const auto bytes = require_duration_bytes(arguments, 0);
        return PulseOutputWriteRequest{duration_sequence(*bytes)};
    }
    case Capability::spi_peripheral:
        require_arity(arguments, 1);
        return SpiPeripheralWriteRequest{bytes_argument(arguments, 0)};
    case Capability::i2c_controller:
        require_arity(arguments, 3);
        return I2cWriteRequest{unsigned_argument<std::uint16_t>(arguments, 0),
                               bool_argument(arguments, 1), bytes_argument(arguments, 2)};
    case Capability::i2c_peripheral:
        require_arity(arguments, 1);
        return I2cPeripheralWriteRequest{bytes_argument(arguments, 0)};
    case Capability::uart:
    case Capability::rs485:
        require_arity(arguments, 1);
        return UartWriteRequest{bytes_argument(arguments, 0)};
    case Capability::i2s_transmit:
        require_arity(arguments, 1);
        return PcmWriteRequest{bytes_argument(arguments, 0)};
    case Capability::ble_central:
    case Capability::ble_peripheral:
        require_arity(arguments, 4);
        return BleWriteRequest{attribute_arguments(arguments, 0),
                               bounded_enum<BleWriteMode>(arguments, 2, 1),
                               bytes_argument(arguments, 3)};
    case Capability::usb_device:
    case Capability::usb_host:
        require_arity(arguments, 3);
        return UsbWriteRequest{unsigned_argument<std::uint8_t>(arguments, 0),
                               bytes_argument(arguments, 1),
                               unsigned_argument<std::uint32_t>(arguments, 2)};
    default: device_error("capability does not support write");
    }
}

DeviceRequest transfer_request(Capability capability, std::span<const Value> arguments) {
    switch (capability) {
    case Capability::spi_controller:
    case Capability::spi_peripheral:
    case Capability::spi_multilane:
        require_arity(arguments, 2);
        return SpiTransferRequest{bytes_argument(arguments, 0), receive_size(arguments, 1)};
    case Capability::i2c_controller:
        require_arity(arguments, 5);
        return I2cTransferRequest{unsigned_argument<std::uint16_t>(arguments, 0),
                                  bool_argument(arguments, 1), bytes_argument(arguments, 2),
                                  receive_size(arguments, 3), bool_argument(arguments, 4)};
    case Capability::i2s_transmit:
    case Capability::i2s_receive:
        require_arity(arguments, 2);
        return PcmTransferRequest{bytes_argument(arguments, 0), frame_limit(arguments, 1)};
    case Capability::usb_device:
    case Capability::usb_host:
        return usb_transfer_arguments(arguments);
    default: device_error("capability does not support transfer");
    }
}

DeviceRequest subscribe_request(Capability capability, std::span<const Value> arguments) {
    switch (capability) {
    case Capability::gpio:
        require_arity(arguments, 1);
        return GpioSubscribeRequest{bounded_enum<GpioEdge>(arguments, 0, 2)};
    case Capability::ble_central:
    case Capability::ble_peripheral: {
        require_arity(arguments, 3);
        const auto target = bounded_enum<BleSubscriptionTarget>(arguments, 0, 1);
        const auto& service = bytes_argument(arguments, 1);
        const auto& characteristic = bytes_argument(arguments, 2);
        if (target == BleSubscriptionTarget::connection_state) {
            if (!service->empty() || !characteristic->empty()) {
                device_error("connection-state subscription requires empty UUID filters");
            }
            return BleSubscribeRequest{target, {}};
        }
        return BleSubscribeRequest{target, attribute_arguments(arguments, 1)};
    }
    default: device_error("capability does not support subscribe");
    }
}

DeviceRequest discover_request(Capability capability, std::span<const Value> arguments) {
    switch (capability) {
    case Capability::ble_central: {
        require_arity(arguments, 4);
        const auto kind = bounded_enum<BleDiscoveryKind>(arguments, 0, 2);
        const auto& service = bytes_argument(arguments, 1);
        const auto& characteristic = bytes_argument(arguments, 2);
        if (kind == BleDiscoveryKind::scan && (!service->empty() || !characteristic->empty())) {
            device_error("BLE scan requires empty UUID filters");
        }
        const std::uint32_t maximum = kind == BleDiscoveryKind::scan
                                          ? discovery_size<BlePeer>(arguments, 3)
                                          : discovery_size<BleCharacteristicDescriptor>(arguments, 3);
        return BleDiscoverRequest{kind, attribute_arguments(arguments, 1, true), maximum};
    }
    case Capability::usb_host:
        require_arity(arguments, 1);
        return UsbDiscoverRequest{discovery_size<UsbDeviceDescription>(arguments, 0)};
    default: device_error("capability does not support discover");
    }
}

DeviceRequest connect_request(Capability capability, std::span<const Value> arguments) {
    switch (capability) {
    case Capability::ble_central:
        require_arity(arguments, 3);
        return BleConnectRequest{address_argument(arguments, 0, 1),
                                 unsigned_argument<std::uint32_t>(arguments, 2)};
    case Capability::usb_host:
        require_arity(arguments, 1);
        return UsbConnectRequest{unsigned_argument<std::uint8_t>(arguments, 0)};
    default: device_error("capability does not support connect");
    }
}

DeviceRequest disconnect_request(Capability capability, std::span<const Value> arguments) {
    switch (capability) {
    case Capability::ble_central:
        require_arity(arguments, 2);
        return BleDisconnectRequest{address_argument(arguments, 0, 1)};
    case Capability::usb_host:
        require_arity(arguments, 1);
        return UsbDisconnectRequest{unsigned_argument<std::uint8_t>(arguments, 0)};
    default: device_error("capability does not support disconnect");
    }
}

DeviceRequest make_request(DeviceOperation operation, Capability capability,
                           std::span<const Value> arguments) {
    switch (operation) {
    case DeviceOperation::configure: return configure_request(capability, arguments);
    case DeviceOperation::start:
        require_arity(arguments, 0);
        return StartRequest{};
    case DeviceOperation::stop:
        require_arity(arguments, 0);
        return StopRequest{};
    case DeviceOperation::close:
        require_arity(arguments, 0);
        return CloseRequest{};
    case DeviceOperation::read: return read_request(capability, arguments);
    case DeviceOperation::write: return write_request(capability, arguments);
    case DeviceOperation::transfer: return transfer_request(capability, arguments);
    case DeviceOperation::subscribe: return subscribe_request(capability, arguments);
    case DeviceOperation::unsubscribe:
        require_arity(arguments, 1);
        return UnsubscribeRequest{handle_argument(arguments, 0)};
    case DeviceOperation::notify:
        if (capability != Capability::ble_peripheral) device_error("notify requires ble_peripheral");
        require_arity(arguments, 3);
        return BleNotifyRequest{attribute_arguments(arguments, 0), bytes_argument(arguments, 2)};
    case DeviceOperation::connect: return connect_request(capability, arguments);
    case DeviceOperation::disconnect: return disconnect_request(capability, arguments);
    case DeviceOperation::discover: return discover_request(capability, arguments);
    case DeviceOperation::advertise:
        if (capability != Capability::ble_peripheral) device_error("advertise requires ble_peripheral");
        require_arity(arguments, 2);
        return BleAdvertiseRequest{bytes_argument(arguments, 0),
                                   unsigned_argument<std::uint32_t>(arguments, 1)};
    default: device_error("operation cannot be submitted on a device handle");
    }
}

Value bytes_or_integer(const ByteBufferPtr& bytes, std::uint64_t alternative) {
    return bytes ? Value::from_bytes(bytes) : integer_result(alternative);
}

Value primary_result(const DeviceResult& result) {
    if (result.status != IoStatus::completed) return Value::from_void();
    return std::visit(
        [](const auto& response) -> Value {
            using Response = std::decay_t<decltype(response)>;
            if constexpr (std::is_same_v<Response, std::monostate> ||
                          std::is_same_v<Response, EmptyResponse>) {
                return Value::from_void();
            } else if constexpr (std::is_same_v<Response, OpenResponse>) {
                return Value::from_handle(response.device);
            } else if constexpr (std::is_same_v<Response, AdcConfigureResponse>) {
                return integer_result(response.actual_sample_time_ns);
            } else if constexpr (std::is_same_v<Response, PwmConfigureResponse>) {
                return integer_result(response.actual_period_ns);
            } else if constexpr (std::is_same_v<Response, SpiConfigureResponse>) {
                return integer_result(response.actual_frequency_hz);
            } else if constexpr (std::is_same_v<Response, I2cConfigureResponse>) {
                return integer_result(response.actual_frequency_hz);
            } else if constexpr (std::is_same_v<Response, UartConfigureResponse>) {
                return integer_result(response.actual_baud);
            } else if constexpr (std::is_same_v<Response, PulseCaptureConfigureResponse>) {
                return integer_result(response.actual_minimum_duration_ns);
            } else if constexpr (std::is_same_v<Response, PulseOutputConfigureResponse>) {
                return integer_result(response.actual_resolution_ns);
            } else if constexpr (std::is_same_v<Response, PcmConfigureResponse>) {
                return integer_result(response.actual_format.frame_rate_hz);
            } else if constexpr (std::is_same_v<Response, UsbConfigureResponse>) {
                return integer_result(response.actual_configuration);
            } else if constexpr (std::is_same_v<Response, GpioReadResponse>) {
                return Value::from_bool(response.high);
            } else if constexpr (std::is_same_v<Response, AdcReadResponse>) {
                return Value::from_integer(response.sample);
            } else if constexpr (std::is_same_v<Response, SpiTransferResponse>) {
                return bytes_or_integer(response.received, response.bytes_transferred);
            } else if constexpr (std::is_same_v<Response, I2cTransferResponse>) {
                return bytes_or_integer(response.received, response.bytes_written);
            } else if constexpr (std::is_same_v<Response, UartReadResponse>) {
                return response.bytes ? Value::from_bytes(response.bytes) : Value::from_void();
            } else if constexpr (std::is_same_v<Response, UartWriteResponse>) {
                return count_result(response.bytes_written);
            } else if constexpr (std::is_same_v<Response, PulseCaptureReadResponse>) {
                return count_result(response.pulses.size());
            } else if constexpr (std::is_same_v<Response, PulseOutputWriteResponse>) {
                return integer_result(response.transitions_written);
            } else if constexpr (std::is_same_v<Response, PcmReadResponse>) {
                return bytes_or_integer(response.frames, response.frame_count);
            } else if constexpr (std::is_same_v<Response, PcmWriteResponse>) {
                return integer_result(response.frames_consumed);
            } else if constexpr (std::is_same_v<Response, BleScanResponse>) {
                return count_result(response.peers.size());
            } else if constexpr (std::is_same_v<Response, BleGattDiscoveryResponse>) {
                return count_result(response.characteristics.size());
            } else if constexpr (std::is_same_v<Response, BleConnectionResponse>) {
                return Value::from_bool(response.connected);
            } else if constexpr (std::is_same_v<Response, BleReadResponse>) {
                return response.value ? Value::from_bytes(response.value) : Value::from_void();
            } else if constexpr (std::is_same_v<Response, BleWriteResponse>) {
                return count_result(response.bytes_written);
            } else if constexpr (std::is_same_v<Response, SubscriptionResponse>) {
                return Value::from_handle(response.subscription);
            } else if constexpr (std::is_same_v<Response, EventLinkReadResponse>) {
                return Value::from_bool(response.enabled);
            } else if constexpr (std::is_same_v<Response, UsbDiscoveryResponse>) {
                return count_result(response.devices.size());
            } else if constexpr (std::is_same_v<Response, UsbTransferResponse>) {
                return bytes_or_integer(response.received, response.bytes_transferred);
            } else if constexpr (std::is_same_v<Response, UsbWriteResponse>) {
                return count_result(response.bytes_written);
            }
        },
        result.response);
}

void require_no_selection(const std::optional<std::size_t>& selection) {
    if (selection) device_error("metadata field does not accept an index");
}

std::size_t require_selection(const std::optional<std::size_t>& selection, std::size_t size) {
    if (!selection) device_error("metadata list field requires an index");
    if (*selection >= size) device_error("metadata list index is out of bounds");
    return *selection;
}

template <typename Response>
const Response& response_as(const DeviceResult& result) {
    const auto* response = std::get_if<Response>(&result.response);
    if (!response) device_error("metadata field is missing for this result");
    return *response;
}

Value actual_bytes(const ByteBufferPtr& bytes) {
    if (!bytes) device_error("metadata byte field is absent");
    return Value::from_bytes(bytes);
}

Value uuid_bytes(const BleUuid& uuid) {
    if (uuid.length != 2 && uuid.length != 16) device_error("metadata UUID has an invalid length");
    return Value::from_bytes(ByteBuffer(uuid.bytes.begin(), uuid.bytes.begin() + uuid.length));
}

Value address_bytes(const BleAddress& address) {
    return Value::from_bytes(ByteBuffer(address.bytes.begin(), address.bytes.end()));
}

std::size_t encoded_size(std::size_t entries, std::size_t words_per_entry) {
    if (words_per_entry != 0 && entries > std::numeric_limits<std::size_t>::max() / words_per_entry) {
        device_error("metadata sequence size overflows");
    }
    const std::size_t words = entries * words_per_entry;
    if (words > std::numeric_limits<std::size_t>::max() / sizeof(std::uint64_t)) {
        device_error("metadata sequence byte size overflows");
    }
    const std::size_t bytes = words * sizeof(std::uint64_t);
    if (bytes > device_budget.maximum_bytes_per_buffer) {
        device_error("metadata sequence exceeds maximum_bytes_per_buffer");
    }
    return bytes;
}

void append_little_endian_u64(ByteBuffer& output, std::uint64_t value) {
    for (unsigned byte = 0; byte < sizeof(std::uint64_t); ++byte) {
        output.push_back(static_cast<std::uint8_t>(value >> (byte * 8U)));
    }
}

enum class PulseEncoding : std::uint8_t { samples, durations, timestamps };

Value encoded_pulses(const PulseCaptureReadResponse& response, PulseEncoding encoding) {
    const std::size_t words = encoding == PulseEncoding::samples ? 3 : 1;
    ByteBuffer output;
    output.reserve(encoded_size(response.pulses.size(), words));
    for (const PulseSample& pulse : response.pulses) {
        if (encoding != PulseEncoding::timestamps) {
            append_little_endian_u64(output, std::bit_cast<std::uint64_t>(pulse.duration_ns));
        }
        if (pulse.level != PulseLevel::low && pulse.level != PulseLevel::high) {
            device_error("pulse result contains an invalid level");
        }
        if (encoding == PulseEncoding::samples) {
            append_little_endian_u64(output, std::bit_cast<std::uint64_t>(pulse.timestamp_ns));
            append_little_endian_u64(output, pulse.level == PulseLevel::high ? 1 : 0);
        } else if (encoding == PulseEncoding::timestamps) {
            append_little_endian_u64(output, std::bit_cast<std::uint64_t>(pulse.timestamp_ns));
        }
    }
    return Value::from_bytes(std::move(output));
}

const BlePeer& selected_peer(const DeviceResult& result,
                             const std::optional<std::size_t>& selection) {
    const auto& response = response_as<BleScanResponse>(result);
    return response.peers[require_selection(selection, response.peers.size())];
}

const BleCharacteristicDescriptor& selected_characteristic(
    const DeviceResult& result, const std::optional<std::size_t>& selection) {
    const auto& response = response_as<BleGattDiscoveryResponse>(result);
    return response.characteristics[require_selection(selection, response.characteristics.size())];
}

const UsbDeviceDescription& selected_usb_device(const DeviceResult& result,
                                                const std::optional<std::size_t>& selection) {
    const auto& response = response_as<UsbDiscoveryResponse>(result);
    return response.devices[require_selection(selection, response.devices.size())];
}

Value metadata_value(const DeviceResult& result, std::uint16_t raw_field,
                     std::optional<std::size_t> selection) {
    const auto field = static_cast<DeviceMetadataField>(raw_field);
    switch (field) {
    case DeviceMetadataField::io_status:
        require_no_selection(selection);
        return Value::from_integer(static_cast<std::int64_t>(result.status));
    case DeviceMetadataField::timestamp_ns:
        require_no_selection(selection);
        if (const auto* value = std::get_if<GpioReadResponse>(&result.response)) {
            return Value::from_integer(value->timestamp_ns);
        }
        if (const auto* value = std::get_if<AdcReadResponse>(&result.response)) {
            return Value::from_integer(value->timestamp_ns);
        }
        return Value::from_integer(response_as<UartReadResponse>(result).timestamp_ns);
    case DeviceMetadataField::sequence:
        require_no_selection(selection);
        return Value::from_integer(response_as<PcmReadResponse>(result).sequence);
    case DeviceMetadataField::modeled:
        require_no_selection(selection);
        return Value::from_bool(response_as<OpenResponse>(result).modeled);
    case DeviceMetadataField::capability_major:
        require_no_selection(selection);
        return integer_result(response_as<OpenResponse>(result).version.major);
    case DeviceMetadataField::capability_minor:
        require_no_selection(selection);
        return integer_result(response_as<OpenResponse>(result).version.minor);
    case DeviceMetadataField::actual_sample_time_ns:
        require_no_selection(selection);
        return integer_result(response_as<AdcConfigureResponse>(result).actual_sample_time_ns);
    case DeviceMetadataField::actual_reference_microvolts:
        require_no_selection(selection);
        return integer_result(response_as<AdcConfigureResponse>(result).actual_reference_microvolts);
    case DeviceMetadataField::reference_microvolts:
        require_no_selection(selection);
        return Value::from_integer(response_as<AdcReadResponse>(result).reference_microvolts);
    case DeviceMetadataField::resolution_bits:
        require_no_selection(selection);
        if (const auto* value = std::get_if<AdcConfigureResponse>(&result.response)) {
            return integer_result(value->resolution_bits);
        }
        return integer_result(response_as<AdcReadResponse>(result).resolution_bits);
    case DeviceMetadataField::calibrated:
        require_no_selection(selection);
        if (const auto* value = std::get_if<AdcConfigureResponse>(&result.response)) {
            return Value::from_bool(value->calibrated);
        }
        return Value::from_bool(response_as<AdcReadResponse>(result).calibrated);
    case DeviceMetadataField::actual_period_ns:
        require_no_selection(selection);
        return integer_result(response_as<PwmConfigureResponse>(result).actual_period_ns);
    case DeviceMetadataField::actual_frequency_hz:
        require_no_selection(selection);
        if (const auto* value = std::get_if<SpiConfigureResponse>(&result.response)) {
            return integer_result(value->actual_frequency_hz);
        }
        return integer_result(response_as<I2cConfigureResponse>(result).actual_frequency_hz);
    case DeviceMetadataField::actual_baud:
        require_no_selection(selection);
        return integer_result(response_as<UartConfigureResponse>(result).actual_baud);
    case DeviceMetadataField::actual_minimum_duration_ns:
        require_no_selection(selection);
        return integer_result(response_as<PulseCaptureConfigureResponse>(result).actual_minimum_duration_ns);
    case DeviceMetadataField::actual_resolution_ns:
        require_no_selection(selection);
        return integer_result(response_as<PulseOutputConfigureResponse>(result).actual_resolution_ns);
    case DeviceMetadataField::frame_rate_hz:
        require_no_selection(selection);
        return integer_result(response_as<PcmConfigureResponse>(result).actual_format.frame_rate_hz);
    case DeviceMetadataField::channel_count:
        require_no_selection(selection);
        return integer_result(response_as<PcmConfigureResponse>(result).actual_format.channel_count);
    case DeviceMetadataField::pcm_sample_format: {
        require_no_selection(selection);
        const auto format = response_as<PcmConfigureResponse>(result).actual_format.sample_format;
        const auto raw = static_cast<std::uint8_t>(format);
        if (raw > static_cast<std::uint8_t>(PcmSampleFormat::float_32_le)) {
            device_error("PCM result contains an invalid sample format");
        }
        return integer_result(raw);
    }
    case DeviceMetadataField::actual_configuration:
        require_no_selection(selection);
        return integer_result(response_as<UsbConfigureResponse>(result).actual_configuration);
    case DeviceMetadataField::sample:
        require_no_selection(selection);
        return Value::from_integer(response_as<AdcReadResponse>(result).sample);
    case DeviceMetadataField::received:
        require_no_selection(selection);
        if (const auto* value = std::get_if<SpiTransferResponse>(&result.response)) {
            return actual_bytes(value->received);
        }
        if (const auto* value = std::get_if<I2cTransferResponse>(&result.response)) {
            return actual_bytes(value->received);
        }
        return actual_bytes(response_as<UsbTransferResponse>(result).received);
    case DeviceMetadataField::bytes_transferred:
        require_no_selection(selection);
        if (const auto* value = std::get_if<SpiTransferResponse>(&result.response)) {
            return count_result(value->bytes_transferred);
        }
        return count_result(response_as<UsbTransferResponse>(result).bytes_transferred);
    case DeviceMetadataField::bytes_written:
        require_no_selection(selection);
        if (const auto* value = std::get_if<I2cTransferResponse>(&result.response)) {
            return count_result(value->bytes_written);
        }
        if (const auto* value = std::get_if<UartWriteResponse>(&result.response)) {
            return count_result(value->bytes_written);
        }
        if (const auto* value = std::get_if<BleWriteResponse>(&result.response)) {
            return count_result(value->bytes_written);
        }
        return count_result(response_as<UsbWriteResponse>(result).bytes_written);
    case DeviceMetadataField::i2c_fault:
        require_no_selection(selection);
        return integer_result(static_cast<std::uint8_t>(response_as<I2cTransferResponse>(result).fault));
    case DeviceMetadataField::repeated_start:
        require_no_selection(selection);
        return Value::from_bool(response_as<I2cTransferResponse>(result).repeated_start);
    case DeviceMetadataField::uart_bytes:
        require_no_selection(selection);
        return actual_bytes(response_as<UartReadResponse>(result).bytes);
    case DeviceMetadataField::uart_error_overflow:
        require_no_selection(selection);
        return Value::from_bool(response_as<UartReadResponse>(result).errors.overflow);
    case DeviceMetadataField::uart_error_framing:
        require_no_selection(selection);
        return Value::from_bool(response_as<UartReadResponse>(result).errors.framing);
    case DeviceMetadataField::uart_error_parity:
        require_no_selection(selection);
        return Value::from_bool(response_as<UartReadResponse>(result).errors.parity);
    case DeviceMetadataField::uart_error_break_detected:
        require_no_selection(selection);
        return Value::from_bool(response_as<UartReadResponse>(result).errors.break_detected);
    case DeviceMetadataField::pulse_count:
        require_no_selection(selection);
        return count_result(response_as<PulseCaptureReadResponse>(result).pulses.size());
    case DeviceMetadataField::pulse_samples_le64:
        require_no_selection(selection);
        return encoded_pulses(response_as<PulseCaptureReadResponse>(result), PulseEncoding::samples);
    case DeviceMetadataField::durations_ns_le64:
        require_no_selection(selection);
        return encoded_pulses(response_as<PulseCaptureReadResponse>(result), PulseEncoding::durations);
    case DeviceMetadataField::pulse_timestamps_ns_le64:
        require_no_selection(selection);
        return encoded_pulses(response_as<PulseCaptureReadResponse>(result), PulseEncoding::timestamps);
    case DeviceMetadataField::transitions_written:
        require_no_selection(selection);
        return integer_result(response_as<PulseOutputWriteResponse>(result).transitions_written);
    case DeviceMetadataField::pcm_frames:
        require_no_selection(selection);
        return actual_bytes(response_as<PcmReadResponse>(result).frames);
    case DeviceMetadataField::frame_count:
        require_no_selection(selection);
        return integer_result(response_as<PcmReadResponse>(result).frame_count);
    case DeviceMetadataField::frames_consumed:
        require_no_selection(selection);
        return integer_result(response_as<PcmWriteResponse>(result).frames_consumed);
    case DeviceMetadataField::peer_count:
        require_no_selection(selection);
        return count_result(response_as<BleScanResponse>(result).peers.size());
    case DeviceMetadataField::peer_address:
        return address_bytes(selected_peer(result, selection).address);
    case DeviceMetadataField::peer_address_random:
        return Value::from_bool(selected_peer(result, selection).address.random);
    case DeviceMetadataField::received_signal_strength_dbm:
        return Value::from_integer(selected_peer(result, selection).received_signal_strength_dbm);
    case DeviceMetadataField::advertisement:
        return actual_bytes(selected_peer(result, selection).advertisement);
    case DeviceMetadataField::characteristic_count:
        require_no_selection(selection);
        return count_result(response_as<BleGattDiscoveryResponse>(result).characteristics.size());
    case DeviceMetadataField::service_uuid:
        return uuid_bytes(selected_characteristic(result, selection).attribute.service);
    case DeviceMetadataField::characteristic_uuid:
        return uuid_bytes(selected_characteristic(result, selection).attribute.characteristic);
    case DeviceMetadataField::readable:
        return Value::from_bool(selected_characteristic(result, selection).readable);
    case DeviceMetadataField::writable:
        return Value::from_bool(selected_characteristic(result, selection).writable);
    case DeviceMetadataField::notifiable:
        return Value::from_bool(selected_characteristic(result, selection).notifiable);
    case DeviceMetadataField::connected:
        require_no_selection(selection);
        return Value::from_bool(response_as<BleConnectionResponse>(result).connected);
    case DeviceMetadataField::connected_peer_address:
        require_no_selection(selection);
        return address_bytes(response_as<BleConnectionResponse>(result).peer);
    case DeviceMetadataField::connected_peer_address_random:
        require_no_selection(selection);
        return Value::from_bool(response_as<BleConnectionResponse>(result).peer.random);
    case DeviceMetadataField::paired:
        require_no_selection(selection);
        return Value::from_bool(response_as<BleConnectionResponse>(result).paired);
    case DeviceMetadataField::attribute_service_uuid:
        require_no_selection(selection);
        return uuid_bytes(response_as<BleReadResponse>(result).attribute.service);
    case DeviceMetadataField::attribute_characteristic_uuid:
        require_no_selection(selection);
        return uuid_bytes(response_as<BleReadResponse>(result).attribute.characteristic);
    case DeviceMetadataField::ble_value:
        require_no_selection(selection);
        return actual_bytes(response_as<BleReadResponse>(result).value);
    case DeviceMetadataField::enabled:
        require_no_selection(selection);
        return Value::from_bool(response_as<EventLinkReadResponse>(result).enabled);
    case DeviceMetadataField::hardware_autonomous:
        require_no_selection(selection);
        return Value::from_bool(response_as<EventLinkReadResponse>(result).hardware_autonomous);
    case DeviceMetadataField::trigger_count:
        require_no_selection(selection);
        return Value::from_integer(response_as<EventLinkReadResponse>(result).trigger_count);
    case DeviceMetadataField::usb_device_count:
        require_no_selection(selection);
        return count_result(response_as<UsbDiscoveryResponse>(result).devices.size());
    case DeviceMetadataField::usb_device_address:
        return integer_result(selected_usb_device(result, selection).address);
    case DeviceMetadataField::usb_vendor_id:
        return integer_result(selected_usb_device(result, selection).vendor_id);
    case DeviceMetadataField::usb_product_id:
        return integer_result(selected_usb_device(result, selection).product_id);
    case DeviceMetadataField::usb_device_descriptor:
        return actual_bytes(selected_usb_device(result, selection).device_descriptor);
    }
    device_error("unknown metadata field " + std::to_string(raw_field));
}

Value native_integer(const Value& value) {
    return Value::from_integer(integer_value(value, true));
}

Value native_number(const Value& value) {
    if (value.type == Value::Type::number) {
        if (!std::isfinite(value.number)) device_error("number must be finite");
        return value;
    }
    if (value.type != Value::Type::integer) device_error("number expects int64 or f32");
    const float converted = static_cast<float>(value.integer);
    if (!std::isfinite(converted) || static_cast<double>(converted) < -9223372036854775808.0 ||
        static_cast<double>(converted) >= 9223372036854775808.0 ||
        static_cast<std::int64_t>(converted) != value.integer) {
        device_error("int64 is not exactly representable as f32");
    }
    return Value::from_number(converted);
}

std::uint8_t hex_nibble(char character) {
    if (character >= '0' && character <= '9') return static_cast<std::uint8_t>(character - '0');
    if (character >= 'a' && character <= 'f') return static_cast<std::uint8_t>(character - 'a' + 10);
    if (character >= 'A' && character <= 'F') return static_cast<std::uint8_t>(character - 'A' + 10);
    device_error("hex string contains a non-ASCII-hex character");
}

Value native_bytes(std::span<const Value> arguments) {
    if (arguments.empty()) return Value::from_bytes(ByteBuffer{});
    require_arity(arguments, 1);
    const std::string& hex = string_argument(arguments, 0);
    if (hex.size() % 2 != 0) device_error("hex string must have even length");
    const std::size_t output_size = hex.size() / 2;
    if (output_size > device_budget.maximum_bytes_per_buffer) {
        device_error("hex decoding exceeds maximum_bytes_per_buffer");
    }
    ByteBuffer output;
    output.reserve(output_size);
    for (std::size_t index = 0; index < hex.size(); index += 2) {
        output.push_back(static_cast<std::uint8_t>((hex_nibble(hex[index]) << 4U) |
                                                   hex_nibble(hex[index + 1])));
    }
    return Value::from_bytes(std::move(output));
}

std::uint8_t one_byte_value(const Value& value) {
    return static_cast<std::uint8_t>(unsigned_value(value, 255));
}

Value native_append_byte(std::span<const Value> arguments) {
    require_arity(arguments, 2);
    const auto& source = bytes_argument(arguments, 0);
    if (source->size() == device_budget.maximum_bytes_per_buffer) {
        device_error("append_byte exceeds maximum_bytes_per_buffer");
    }
    ByteBuffer output = *source;
    output.push_back(one_byte_value(argument_at(arguments, 1)));
    return Value::from_bytes(std::move(output));
}

Value native_concatenate(std::span<const Value> arguments) {
    require_arity(arguments, 2);
    const auto& left = bytes_argument(arguments, 0);
    const auto& right = bytes_argument(arguments, 1);
    if (right->size() > device_budget.maximum_bytes_per_buffer - left->size()) {
        device_error("concatenated bytes exceed maximum_bytes_per_buffer");
    }
    ByteBuffer output;
    output.reserve(left->size() + right->size());
    output.insert(output.end(), left->begin(), left->end());
    output.insert(output.end(), right->begin(), right->end());
    return Value::from_bytes(std::move(output));
}

Value native_byte_length(std::span<const Value> arguments) {
    require_arity(arguments, 1);
    return count_result(bytes_argument(arguments, 0)->size());
}

Value native_byte_at(std::span<const Value> arguments) {
    require_arity(arguments, 2);
    const auto& bytes = bytes_argument(arguments, 0);
    const auto index = unsigned_value(argument_at(arguments, 1),
                                      std::numeric_limits<std::size_t>::max());
    if (index >= bytes->size()) device_error("byte_at index is out of bounds");
    return integer_result((*bytes)[static_cast<std::size_t>(index)]);
}

std::string status_rejection(DeviceOperation operation, IoStatus status) {
    return "operation " + std::to_string(static_cast<std::uint16_t>(operation)) +
           " rejected with IoStatus " + std::to_string(static_cast<std::uint8_t>(status));
}

[[nodiscard]] bool open_version_supported(const EndpointMetadata& endpoint,
                                          CapabilityVersion minimum) noexcept {
    const CapabilityVersion available = endpoint.descriptor.version;
    return available.capability == minimum.capability &&
           available.major == peripheral_abi_major && available.major == minimum.major &&
           available.minor >= minimum.minor && endpoint.supports(DeviceOperation::open);
}

[[nodiscard]] const CapabilityVersion* find_declaration(const Package& package,
                                                        Capability capability) noexcept {
    const auto required = std::find_if(package.required_capabilities.begin(),
        package.required_capabilities.end(), [capability](const CapabilityVersion& declaration) {
            return declaration.capability == capability;
        });
    if (required != package.required_capabilities.end()) return &*required;
    const auto optional = std::find_if(package.optional_capabilities.begin(),
        package.optional_capabilities.end(), [capability](const CapabilityVersion& declaration) {
            return declaration.capability == capability;
        });
    return optional == package.optional_capabilities.end() ? nullptr : &*optional;
}

} // namespace

std::string_view device_metadata_field_name(DeviceMetadataField field) {
    switch (field) {
    case DeviceMetadataField::io_status: return "io_status";
    case DeviceMetadataField::timestamp_ns: return "timestamp_ns";
    case DeviceMetadataField::sequence: return "sequence";
    case DeviceMetadataField::modeled: return "modeled";
    case DeviceMetadataField::capability_major: return "capability_major";
    case DeviceMetadataField::capability_minor: return "capability_minor";
    case DeviceMetadataField::actual_sample_time_ns: return "actual_sample_time_ns";
    case DeviceMetadataField::actual_reference_microvolts: return "actual_reference_microvolts";
    case DeviceMetadataField::reference_microvolts: return "reference_microvolts";
    case DeviceMetadataField::resolution_bits: return "resolution_bits";
    case DeviceMetadataField::calibrated: return "calibrated";
    case DeviceMetadataField::actual_period_ns: return "actual_period_ns";
    case DeviceMetadataField::actual_frequency_hz: return "actual_frequency_hz";
    case DeviceMetadataField::actual_baud: return "actual_baud";
    case DeviceMetadataField::actual_minimum_duration_ns: return "actual_minimum_duration_ns";
    case DeviceMetadataField::actual_resolution_ns: return "actual_resolution_ns";
    case DeviceMetadataField::frame_rate_hz: return "frame_rate_hz";
    case DeviceMetadataField::channel_count: return "channel_count";
    case DeviceMetadataField::pcm_sample_format: return "pcm_sample_format";
    case DeviceMetadataField::actual_configuration: return "actual_configuration";
    case DeviceMetadataField::sample: return "sample";
    case DeviceMetadataField::received: return "received";
    case DeviceMetadataField::bytes_transferred: return "bytes_transferred";
    case DeviceMetadataField::bytes_written: return "bytes_written";
    case DeviceMetadataField::i2c_fault: return "i2c_fault";
    case DeviceMetadataField::repeated_start: return "repeated_start";
    case DeviceMetadataField::uart_bytes: return "uart_bytes";
    case DeviceMetadataField::uart_error_overflow: return "uart_error_overflow";
    case DeviceMetadataField::uart_error_framing: return "uart_error_framing";
    case DeviceMetadataField::uart_error_parity: return "uart_error_parity";
    case DeviceMetadataField::uart_error_break_detected: return "uart_error_break_detected";
    case DeviceMetadataField::pulse_count: return "pulse_count";
    case DeviceMetadataField::pulse_samples_le64: return "pulse_samples_le64";
    case DeviceMetadataField::durations_ns_le64: return "durations_ns_le64";
    case DeviceMetadataField::pulse_timestamps_ns_le64: return "pulse_timestamps_ns_le64";
    case DeviceMetadataField::transitions_written: return "transitions_written";
    case DeviceMetadataField::pcm_frames: return "pcm_frames";
    case DeviceMetadataField::frame_count: return "frame_count";
    case DeviceMetadataField::frames_consumed: return "frames_consumed";
    case DeviceMetadataField::peer_count: return "peer_count";
    case DeviceMetadataField::peer_address: return "peer_address";
    case DeviceMetadataField::peer_address_random: return "peer_address_random";
    case DeviceMetadataField::received_signal_strength_dbm: return "received_signal_strength_dbm";
    case DeviceMetadataField::advertisement: return "advertisement";
    case DeviceMetadataField::characteristic_count: return "characteristic_count";
    case DeviceMetadataField::service_uuid: return "service_uuid";
    case DeviceMetadataField::characteristic_uuid: return "characteristic_uuid";
    case DeviceMetadataField::readable: return "readable";
    case DeviceMetadataField::writable: return "writable";
    case DeviceMetadataField::notifiable: return "notifiable";
    case DeviceMetadataField::connected: return "connected";
    case DeviceMetadataField::connected_peer_address: return "connected_peer_address";
    case DeviceMetadataField::connected_peer_address_random: return "connected_peer_address_random";
    case DeviceMetadataField::paired: return "paired";
    case DeviceMetadataField::attribute_service_uuid: return "attribute_service_uuid";
    case DeviceMetadataField::attribute_characteristic_uuid: return "attribute_characteristic_uuid";
    case DeviceMetadataField::ble_value: return "ble_value";
    case DeviceMetadataField::enabled: return "enabled";
    case DeviceMetadataField::hardware_autonomous: return "hardware_autonomous";
    case DeviceMetadataField::trigger_count: return "trigger_count";
    case DeviceMetadataField::usb_device_count: return "usb_device_count";
    case DeviceMetadataField::usb_device_address: return "usb_device_address";
    case DeviceMetadataField::usb_vendor_id: return "usb_vendor_id";
    case DeviceMetadataField::usb_product_id: return "usb_product_id";
    case DeviceMetadataField::usb_device_descriptor: return "usb_device_descriptor";
    }
    throw ScriptError("unknown device metadata field " +
                      std::to_string(static_cast<std::uint16_t>(field)));
}

DeviceSession& Browser::prepare_devices() {
    if (m_devices) return *m_devices;
    if (!m_peripherals) device_error("no peripheral registry is available");
    m_device_permission_names.clear();
    m_device_permission_names.reserve(m_package.permission_endpoints.size());
    for (const std::string& name : m_package.permission_endpoints) {
        m_device_permission_names.emplace_back(name);
    }
    std::vector<CapabilityVersion> capabilities = m_package.required_capabilities;
    capabilities.insert(capabilities.end(), m_package.optional_capabilities.begin(),
                        m_package.optional_capabilities.end());
    const ApplicationPermissions permissions{capabilities, m_device_permission_names};
    try {
        m_devices = m_peripherals->create_session(permissions, device_budget);
    } catch (...) {
        m_device_permission_names.clear();
        throw;
    }
    m_device_callbacks.reserve(device_budget.maximum_pending_operations);
    m_device_subscriptions.reserve(device_budget.maximum_open_handles);
    return *m_devices;
}

void Browser::validate_required_devices() {
    if (m_package.required_capabilities.empty()) return;
    if (!m_peripherals) {
        throw PackageError("required hardware capabilities need a peripheral registry");
    }
    const auto endpoints = prepare_devices().endpoints();
    for (const CapabilityVersion& required : m_package.required_capabilities) {
        const auto found = std::find_if(endpoints.begin(), endpoints.end(),
            [&](const EndpointMetadata& endpoint) { return open_version_supported(endpoint, required); });
        if (found == endpoints.end()) {
            throw PackageError("required capability " +
                std::to_string(static_cast<unsigned>(required.capability)) +
                " has no permissioned endpoint supporting open at ABI major " +
                std::to_string(required.major) + " and minor " + std::to_string(required.minor) +
                " or newer");
        }
    }
}

Value Browser::device_call(std::uint16_t raw_operation, std::span<const Value> arguments) {
    switch (static_cast<DeviceNativeOperation>(raw_operation)) {
    case DeviceNativeOperation::bytes: return native_bytes(arguments);
    case DeviceNativeOperation::bytes_length: return native_byte_length(arguments);
    case DeviceNativeOperation::byte_at: return native_byte_at(arguments);
    case DeviceNativeOperation::integer:
        require_arity(arguments, 1);
        return native_integer(argument_at(arguments, 0));
    case DeviceNativeOperation::number:
        require_arity(arguments, 1);
        return native_number(argument_at(arguments, 0));
    case DeviceNativeOperation::one_byte: {
        require_arity(arguments, 1);
        ByteBuffer output{one_byte_value(argument_at(arguments, 0))};
        return Value::from_bytes(std::move(output));
    }
    case DeviceNativeOperation::append_byte: return native_append_byte(arguments);
    case DeviceNativeOperation::concatenate_bytes: return native_concatenate(arguments);
    case DeviceNativeOperation::usb_setup: return native_usb_setup(arguments);
    case DeviceNativeOperation::metadata: {
        if (arguments.size() != 2 && arguments.size() != 3) {
            device_error("metadata expects token, field, and optional index");
        }
        if (!m_active_device_result) device_error("metadata is only valid inside its device callback");
        const DeviceHandle token = handle_argument(arguments, 0);
        const DeviceHandle active_token =
            m_active_device_result->kind == DeviceResultKind::completion
                ? m_active_device_result->operation
                : m_active_device_result->subscription;
        if (token != active_token) device_error("metadata token does not match the active callback");
        const auto field = unsigned_argument<std::uint16_t>(arguments, 1);
        std::optional<std::size_t> selection;
        if (arguments.size() == 3) {
            selection = static_cast<std::size_t>(unsigned_value(
                argument_at(arguments, 2), std::numeric_limits<std::size_t>::max()));
        }
        return metadata_value(*m_active_device_result, field, selection);
    }
    case DeviceNativeOperation::capability_supported: break;
    default: break;
    }

    auto session = [&]() -> DeviceSession& { return prepare_devices(); };

    if (raw_operation == static_cast<std::uint16_t>(DeviceNativeOperation::capability_supported)) {
        require_arity(arguments, 2);
        const Capability capability = capability_value(arguments, 0);
        const std::string& endpoint = string_argument(arguments, 1);
        const CapabilityVersion* declaration = find_declaration(m_package, capability);
        if (declaration == nullptr || !m_peripherals ||
            std::find(m_package.permission_endpoints.begin(), m_package.permission_endpoints.end(),
                      endpoint) == m_package.permission_endpoints.end()) {
            return Value::from_bool(false);
        }
        // Session endpoints are the aggregate intersection of manifest capability grants,
        // exact permission_endpoints and owner access. The probe selects declaration.minor
        // as open's minimum_minor; an older registered version is not a valid fallback.
        const auto endpoints = session().endpoints();
        const auto found = std::find_if(endpoints.begin(), endpoints.end(), [&](const auto& candidate) {
            return candidate.descriptor.logical_name == endpoint &&
                   open_version_supported(candidate, *declaration);
        });
        return Value::from_bool(found != endpoints.end());
    }

    if (raw_operation == static_cast<std::uint16_t>(DeviceOperation::cancel)) {
        require_arity(arguments, 1);
        const DeviceHandle operation = handle_argument(arguments, 0);
        const IoStatus status = session().cancel(operation);
        if (status != IoStatus::completed) device_error(status_rejection(DeviceOperation::cancel, status));
        return Value::from_void();
    }
    if (raw_operation < static_cast<std::uint16_t>(DeviceOperation::open) ||
        raw_operation > static_cast<std::uint16_t>(DeviceOperation::advertise)) {
        device_error("unknown device operation " + std::to_string(raw_operation));
    }
    const auto operation = static_cast<DeviceOperation>(raw_operation);

    auto submit = [&](std::uint32_t completion_function, std::uint32_t notification_function,
                      DeviceHandle associated_subscription, bool creates_subscription,
                      auto&& action) -> Value {
        if (m_device_callbacks.size() >= device_budget.maximum_pending_operations) {
            device_error("maximum pending device callbacks is full");
        }
        if (creates_subscription &&
            m_device_subscriptions.size() >= device_budget.maximum_open_handles) {
            device_error("maximum device subscription callbacks is full");
        }
        const std::size_t callback_index = m_device_callbacks.size();
        const std::size_t subscription_index = m_device_subscriptions.size();
        try {
            m_device_callbacks.push_back(
                DeviceCallback{{}, associated_subscription, completion_function});
            if (creates_subscription) {
                m_device_subscriptions.push_back(DeviceCallback{{}, {}, notification_function});
            }
            const SubmissionResult submitted = action();
            if (submitted.status != IoStatus::completed || submitted.operation.is_empty()) {
                device_error(status_rejection(operation, submitted.status));
            }
            m_device_callbacks[callback_index].operation = submitted.operation;
            if (creates_subscription) {
                m_device_subscriptions[subscription_index].operation = submitted.operation;
            }
            return Value::from_handle(submitted.operation);
        } catch (...) {
            m_device_callbacks.resize(callback_index);
            if (creates_subscription) m_device_subscriptions.resize(subscription_index);
            throw;
        }
    };

    if (operation == DeviceOperation::open) {
        require_arity(arguments, 4);
        OpenRequest request;
        request.minimum_version = CapabilityVersion{capability_value(arguments, 0), peripheral_abi_major,
                                                    unsigned_argument<std::uint16_t>(arguments, 2)};
        const std::uint32_t callback = find_function(string_argument(arguments, 3));
        if (m_package.functions[callback].parameter_count != device_callback_arity) {
            device_error("device callback must accept exactly three arguments");
        }
        (void)session();
        return submit(callback, no_index, {}, false, [&] {
            return session().open(string_argument(arguments, 1), request);
        });
    }

    if (arguments.size() < 2) device_error("device operation requires handle and callback");
    const DeviceHandle device = handle_argument(arguments, 0);
    const bool subscribing = operation == DeviceOperation::subscribe;
    const std::size_t callback_suffix = subscribing ? 2 : 1;
    if (arguments.size() <= callback_suffix) device_error("missing device callback");
    const std::size_t callback_index = arguments.size() - callback_suffix;
    const std::uint32_t callback = find_function(string_argument(arguments, callback_index));
    if (m_package.functions[callback].parameter_count != device_callback_arity) {
        device_error("device callback must accept exactly three arguments");
    }
    std::uint32_t notification_callback = no_index;
    if (subscribing) {
        notification_callback = find_function(string_argument(arguments, callback_index + 1));
        if (m_package.functions[notification_callback].parameter_count != device_callback_arity) {
            device_error("notification callback must accept exactly three arguments");
        }
    }
    const std::span<const Value> fields = arguments.subspan(1, callback_index - 1);
    DeviceRequest request = make_request(operation, device.capability, fields);
    DeviceHandle associated_subscription;
    if (operation == DeviceOperation::unsubscribe) {
        associated_subscription = std::get<UnsubscribeRequest>(request).subscription;
    }
    (void)session();
    return submit(callback, notification_callback, associated_subscription, subscribing, [&] {
        return session().execute(device, operation, std::move(request));
    });
}

void Browser::invoke(std::uint32_t function, std::span<const Value> arguments) {
    if (function == no_index) return;
    if (function >= m_package.functions.size()) {
        throw ScriptError("handler index is out of bounds");
    }
    if (m_invocations >= 8) {
        handle_error("page on_load navigation exceeds eight levels");
        return;
    }
    ++m_invocations;
    ++m_metrics.scripts;
    struct Restore {
        std::uint32_t& invocations;
        ~Restore() { --invocations; }
    } restore{m_invocations};
    try {
        (void)execute_script(m_package, function, arguments, *this);
    } catch (const Navigation&) {
        cancel_pointer_gesture();
    } catch (const std::exception& error) {
        handle_error(error.what());
    }
}

void Browser::close_devices() {
    m_active_device_result = nullptr;
    std::unique_ptr<DeviceSession> devices = std::move(m_devices);
    m_device_callbacks.clear();
    m_device_subscriptions.clear();
    m_device_permission_names.clear();
    if (!devices) return;
    try {
        devices->request_shutdown();
    } catch (const std::exception& error) {
        handle_error(error.what());
    }
}

void Browser::process_devices() {
    if (!m_devices) return;
    const std::uint64_t generation = m_devices->owner_generation();
    const DeviceBudget budget = m_devices->budget();
    const std::size_t drain_limit =
        budget.maximum_pending_operations + budget.maximum_open_handles;
    for (std::size_t drained = 0; drained < drain_limit; ++drained) {
        if (!m_devices || m_devices->owner_generation() != generation) return;
        try {
            std::optional<DeviceResult> polled = m_devices->poll();
            if (!polled) return;
            DeviceResult& result = *polled;
            const DeviceHandle token = result.kind == DeviceResultKind::completion
                                           ? result.operation
                                           : result.subscription;
            if (token.is_empty() || token.owner_generation != generation) {
                m_platform.log("Discarded stale device result");
                continue;
            }

            DeviceCallback callback{{}, {}, no_index};
            if (result.kind == DeviceResultKind::completion) {
                const auto found = std::find_if(m_device_callbacks.begin(), m_device_callbacks.end(),
                                                [&](const DeviceCallback& candidate) {
                                                    return candidate.operation == result.operation;
                                                });
                if (found == m_device_callbacks.end()) {
                    m_platform.log("Discarded device completion without a live callback");
                    continue;
                }
                callback = *found;
                m_device_callbacks.erase(found);
            } else {
                const auto found = std::find_if(m_device_subscriptions.begin(), m_device_subscriptions.end(),
                                                [&](const DeviceCallback& candidate) {
                                                    return !candidate.subscription.is_empty() &&
                                                           candidate.subscription == result.subscription;
                                                });
                if (found == m_device_subscriptions.end()) {
                    m_platform.log("Discarded device notification without a live callback");
                    continue;
                }
                callback = *found;
            }

            if (result.kind == DeviceResultKind::completion && result.id == DeviceOperation::subscribe) {
                const auto pending = std::find_if(m_device_subscriptions.begin(), m_device_subscriptions.end(),
                                                  [&](const DeviceCallback& candidate) {
                                                      return candidate.subscription.is_empty() &&
                                                             candidate.operation == result.operation;
                                                  });
                if (result.status == IoStatus::completed) {
                    const auto* response = std::get_if<SubscriptionResponse>(&result.response);
                    if (!response || pending == m_device_subscriptions.end()) {
                        if (pending != m_device_subscriptions.end()) m_device_subscriptions.erase(pending);
                        m_platform.log("Discarded incomplete subscription ready result");
                        continue;
                    }
                    pending->subscription = response->subscription;
                    pending->operation = result.device;
                } else if (pending != m_device_subscriptions.end()) {
                    m_device_subscriptions.erase(pending);
                }
            }

            Value primary = primary_result(result);
            Value callback_arguments[] = {Value::from_handle(token),
                Value::from_integer(static_cast<std::int64_t>(result.status)), std::move(primary)};
            m_active_device_result = &result;
            struct ActiveResult {
                const DeviceResult*& slot;
                const DeviceResult* result;
                ~ActiveResult() {
                    if (slot == result) slot = nullptr;
                }
            } active{m_active_device_result, &result};
            invoke(callback.function, callback_arguments);
            if (!m_devices || m_devices->owner_generation() != generation) return;

            if (result.kind == DeviceResultKind::completion) {
                if (result.id == DeviceOperation::unsubscribe) {
                    const auto subscription = std::find_if(
                        m_device_subscriptions.begin(), m_device_subscriptions.end(),
                        [&](const DeviceCallback& candidate) {
                            return candidate.subscription == callback.subscription;
                        });
                    if (subscription != m_device_subscriptions.end()) m_device_subscriptions.erase(subscription);
                } else if (result.id == DeviceOperation::close) {
                    std::erase_if(m_device_subscriptions, [&](const DeviceCallback& candidate) {
                        return !candidate.subscription.is_empty() && candidate.operation == result.device;
                    });
                }
            }
        } catch (const std::exception& error) {
            handle_error(error.what());
            if (!m_devices || m_devices->owner_generation() != generation) return;
        }
    }
}

} // namespace mb
