#pragma once

#include "microbrowser/device_identity.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mb {

inline constexpr std::uint16_t peripheral_abi_major = 1;

// Protocol identifiers are append-only. A backend's registered operation list is
// also the only list exposed through EndpointMetadata.
enum class DeviceOperation : std::uint16_t {
    none = 0,
    open = 1,
    configure = 2,
    start = 3,
    stop = 4,
    close = 5,
    read = 6,
    write = 7,
    transfer = 8,
    subscribe = 9,
    unsubscribe = 10,
    cancel = 11,
    notify = 12,
    connect = 13,
    disconnect = 14,
    discover = 15,
    advertise = 16,
};

enum class EndpointAccess : std::uint8_t { application, owner_only };

struct OpenRequest {
    CapabilityVersion minimum_version = {};
};
struct StartRequest {};
struct StopRequest {};
struct CloseRequest {};
struct UnsubscribeRequest {
    DeviceHandle subscription = {};
};

enum class GpioDirection : std::uint8_t { input, output };
enum class GpioPull : std::uint8_t { none, up, down };
enum class GpioEdge : std::uint8_t { rising, falling, both };

struct GpioConfigureRequest {
    GpioDirection direction = GpioDirection::input;
    GpioPull pull = GpioPull::none;
    bool initial_high = false;
};
struct GpioReadRequest {};
struct GpioWriteRequest {
    bool high = false;
};
struct GpioSubscribeRequest {
    GpioEdge edge = GpioEdge::both;
};

struct AdcConfigureRequest {
    std::uint64_t sample_time_ns = 0;
    std::uint32_t reference_microvolts = 0;
};
struct AdcReadRequest {};

struct PwmConfigureRequest {
    std::uint64_t period_ns = 0;
    std::uint64_t initial_active_time_ns = 0;
};
struct PwmWriteRequest {
    std::uint64_t active_time_ns = 0;
};

enum class SpiMode : std::uint8_t { mode_0, mode_1, mode_2, mode_3 };
enum class BitOrder : std::uint8_t { most_significant_first, least_significant_first };
enum class SpiLaneCount : std::uint8_t { single = 1, dual = 2, quad = 4 };

struct SpiConfigureRequest {
    std::uint32_t frequency_hz = 0;
    SpiMode mode = SpiMode::mode_0;
    BitOrder bit_order = BitOrder::most_significant_first;
    SpiLaneCount lanes = SpiLaneCount::single;
};
struct SpiTransferRequest {
    ByteBufferPtr transmit;
    std::uint32_t receive_bytes = 0;
};
struct SpiPeripheralReadRequest {
    std::uint32_t maximum_bytes = 0;
};
struct SpiPeripheralWriteRequest {
    ByteBufferPtr bytes;
};

struct I2cConfigureRequest {
    std::uint32_t frequency_hz = 0;
};
struct I2cWriteRequest {
    std::uint16_t address = 0;
    bool ten_bit_address = false;
    ByteBufferPtr bytes;
};
struct I2cReadRequest {
    std::uint16_t address = 0;
    bool ten_bit_address = false;
    std::uint32_t byte_count = 0;
};
struct I2cTransferRequest {
    std::uint16_t address = 0;
    bool ten_bit_address = false;
    ByteBufferPtr write_bytes;
    std::uint32_t read_bytes = 0;
    // repeated_start keeps the addressed transaction atomic between its write
    // and read phases. No transaction is split into two driver calls.
    bool repeated_start = true;
};
struct I2cPeripheralReadRequest {
    std::uint32_t maximum_bytes = 0;
};
struct I2cPeripheralWriteRequest {
    ByteBufferPtr bytes;
};

enum class UartParity : std::uint8_t { none, even, odd };
enum class UartFlowControl : std::uint8_t { none, hardware };
enum class Rs485Mode : std::uint8_t { disabled, half_duplex };

struct UartConfigureRequest {
    std::uint32_t baud = 0;
    std::uint8_t data_bits = 8;
    std::uint8_t stop_bits = 1;
    UartParity parity = UartParity::none;
    UartFlowControl flow_control = UartFlowControl::none;
    Rs485Mode rs485_mode = Rs485Mode::disabled;
};
struct UartReadRequest {
    std::uint32_t maximum_bytes = 0;
};
struct UartWriteRequest {
    ByteBufferPtr bytes;
};

enum class PulseLevel : std::uint8_t { low, high };
using NanosecondSequence = std::vector<std::uint64_t>;

struct PulseCaptureConfigureRequest {
    std::uint64_t minimum_duration_ns = 0;
    std::uint32_t maximum_pulses = 0;
};
struct PulseCaptureReadRequest {
    std::uint32_t maximum_pulses = 0;
};
struct PulseOutputConfigureRequest {
    std::uint64_t resolution_ns = 1;
    PulseLevel idle_level = PulseLevel::low;
};
struct PulseOutputWriteRequest {
    std::shared_ptr<const NanosecondSequence> durations_ns;
};

enum class PcmSampleFormat : std::uint8_t { signed_16_le, signed_24_le, signed_32_le, float_32_le };
struct PcmFormat {
    std::uint32_t frame_rate_hz = 0;
    std::uint8_t channel_count = 0;
    PcmSampleFormat sample_format = PcmSampleFormat::signed_16_le;
};
struct PcmConfigureRequest {
    PcmFormat format = {};
    std::uint32_t maximum_frames_per_block = 0;
};
struct PcmReadRequest {
    std::uint32_t frame_count = 0;
};
struct PcmWriteRequest {
    ByteBufferPtr frames;
};
struct PcmTransferRequest {
    ByteBufferPtr transmit_frames;
    std::uint32_t receive_frames = 0;
};

struct BleUuid {
    std::array<std::uint8_t, 16> bytes{};
    std::uint8_t length = 16;

    [[nodiscard]] static BleUuid from_uint16(std::uint16_t value);
};
struct BleAddress {
    std::array<std::uint8_t, 6> bytes{};
    bool random = false;
};
struct BleAttribute {
    BleUuid service = {};
    BleUuid characteristic = {};
};
struct BleCharacteristicDescriptor {
    BleAttribute attribute = {};
    bool readable = false;
    bool writable = false;
    bool notifiable = false;
};
struct BlePeer {
    BleAddress address = {};
    std::int16_t received_signal_strength_dbm = 0;
    ByteBufferPtr advertisement;
};

enum class BleDiscoveryKind : std::uint8_t { scan, services, characteristics };
enum class BleWriteMode : std::uint8_t { with_response, without_response };
enum class BleSubscriptionTarget : std::uint8_t { connection_state, characteristic };

struct BleDiscoverRequest {
    BleDiscoveryKind kind = BleDiscoveryKind::scan;
    BleAttribute filter = {};
    std::uint32_t maximum_results = 0;
};
struct BleConnectRequest {
    BleAddress peer = {};
    std::uint32_t supervision_timeout_ms = 0;
};
struct BleDisconnectRequest {
    BleAddress peer = {};
};
struct BleReadRequest {
    BleAttribute attribute = {};
};
struct BleWriteRequest {
    BleAttribute attribute = {};
    BleWriteMode mode = BleWriteMode::with_response;
    ByteBufferPtr value;
};
struct BleSubscribeRequest {
    BleSubscriptionTarget target = BleSubscriptionTarget::characteristic;
    BleAttribute attribute = {};
};
struct BleAdvertiseRequest {
    ByteBufferPtr payload;
    std::uint32_t interval_ms = 0;
};
struct BleNotifyRequest {
    BleAttribute attribute = {};
    ByteBufferPtr value;
};

// The selected endpoint names an owner-allowlisted event/action route. Raw
// vendor event addresses and channel numbers are deliberately absent.
struct EventLinkReadRequest {};

struct UsbInterfaceDescriptor {
    std::uint8_t interface_number = 0;
    std::uint8_t interface_class = 0;
    std::uint8_t interface_subclass = 0;
    std::uint8_t interface_protocol = 0;
    ByteBufferPtr descriptors;
};
struct UsbDeviceConfigureRequest {
    ByteBufferPtr device_descriptor;
    std::vector<UsbInterfaceDescriptor> interfaces;
};
struct UsbDiscoverRequest {
    std::uint32_t maximum_results = 0;
};
struct UsbConnectRequest {
    std::uint8_t device_address = 0;
};
struct UsbDisconnectRequest {
    std::uint8_t device_address = 0;
};
struct UsbReadRequest {
    std::uint8_t endpoint = 0;
    std::uint32_t byte_count = 0;
    std::uint32_t timeout_ms = 0;
};
struct UsbWriteRequest {
    std::uint8_t endpoint = 0;
    ByteBufferPtr bytes;
    std::uint32_t timeout_ms = 0;
};
struct UsbSetupPacket {
    std::uint8_t request_type = 0;
    std::uint8_t request = 0;
    std::uint16_t value = 0;
    std::uint16_t index = 0;
};
struct UsbTransferRequest {
    UsbSetupPacket setup = {};
    ByteBufferPtr bytes;
    std::uint16_t receive_bytes = 0;
    std::uint32_t timeout_ms = 0;
};

using DeviceRequest = std::variant<
    OpenRequest, StartRequest, StopRequest, CloseRequest, UnsubscribeRequest,
    GpioConfigureRequest, GpioReadRequest, GpioWriteRequest, GpioSubscribeRequest,
    AdcConfigureRequest, AdcReadRequest, PwmConfigureRequest, PwmWriteRequest,
    SpiConfigureRequest, SpiTransferRequest, SpiPeripheralReadRequest, SpiPeripheralWriteRequest,
    I2cConfigureRequest, I2cWriteRequest, I2cReadRequest, I2cTransferRequest,
    I2cPeripheralReadRequest, I2cPeripheralWriteRequest,
    UartConfigureRequest, UartReadRequest, UartWriteRequest,
    PulseCaptureConfigureRequest, PulseCaptureReadRequest, PulseOutputConfigureRequest,
    PulseOutputWriteRequest, PcmConfigureRequest, PcmReadRequest, PcmWriteRequest,
    PcmTransferRequest, BleDiscoverRequest, BleConnectRequest, BleDisconnectRequest,
    BleReadRequest, BleWriteRequest, BleSubscribeRequest, BleAdvertiseRequest, BleNotifyRequest,
    EventLinkReadRequest, UsbDeviceConfigureRequest, UsbDiscoverRequest, UsbConnectRequest,
    UsbDisconnectRequest, UsbReadRequest, UsbWriteRequest, UsbTransferRequest>;

struct OpenResponse {
    DeviceHandle device = {};
    CapabilityVersion version = {};
    bool modeled = false;
};
struct EmptyResponse {};
struct AdcConfigureResponse {
    std::uint64_t actual_sample_time_ns = 0;
    std::uint32_t actual_reference_microvolts = 0;
    std::uint8_t resolution_bits = 0;
    bool calibrated = false;
};
struct PwmConfigureResponse {
    std::uint64_t actual_period_ns = 0;
};
struct SpiConfigureResponse {
    std::uint32_t actual_frequency_hz = 0;
};
struct I2cConfigureResponse {
    std::uint32_t actual_frequency_hz = 0;
};
struct UartConfigureResponse {
    std::uint32_t actual_baud = 0;
};
struct PulseCaptureConfigureResponse {
    std::uint64_t actual_minimum_duration_ns = 0;
};
struct PulseOutputConfigureResponse {
    std::uint64_t actual_resolution_ns = 0;
};
struct PcmConfigureResponse {
    PcmFormat actual_format = {};
};
struct UsbConfigureResponse {
    std::uint8_t actual_configuration = 0;
};
struct GpioReadResponse {
    bool high = false;
    std::int64_t timestamp_ns = 0;
};
struct AdcReadResponse {
    std::int64_t sample = 0;
    std::int64_t reference_microvolts = 0;
    std::uint8_t resolution_bits = 0;
    std::int64_t timestamp_ns = 0;
    bool calibrated = false;
};
struct SpiTransferResponse {
    ByteBufferPtr received;
    std::size_t bytes_transferred = 0;
};

enum class I2cFault : std::uint8_t { none, nack, arbitration_lost, timeout };
struct I2cTransferResponse {
    ByteBufferPtr received;
    std::size_t bytes_written = 0;
    I2cFault fault = I2cFault::none;
    bool repeated_start = false;
};
struct UartLineErrors {
    bool overflow = false;
    bool framing = false;
    bool parity = false;
    bool break_detected = false;
};
struct UartReadResponse {
    ByteBufferPtr bytes;
    std::int64_t timestamp_ns = 0;
    UartLineErrors errors = {};
};
struct UartWriteResponse {
    std::size_t bytes_written = 0;
};
struct PulseSample {
    PulseLevel level = PulseLevel::low;
    std::int64_t duration_ns = 0;
    std::int64_t timestamp_ns = 0;
};
struct PulseCaptureReadResponse {
    std::vector<PulseSample> pulses;
};
struct PulseOutputWriteResponse {
    std::uint64_t transitions_written = 0;
};
struct PcmReadResponse {
    ByteBufferPtr frames;
    std::uint64_t frame_count = 0;
    std::int64_t sequence = 0;
};
struct PcmWriteResponse {
    std::uint64_t frames_consumed = 0;
};
struct BleScanResponse {
    std::vector<BlePeer> peers;
};
struct BleGattDiscoveryResponse {
    std::vector<BleCharacteristicDescriptor> characteristics;
};
struct BleConnectionResponse {
    BleAddress peer = {};
    bool connected = false;
    bool paired = false;
};
struct BleReadResponse {
    BleAttribute attribute = {};
    ByteBufferPtr value;
};
struct BleWriteResponse {
    std::size_t bytes_written = 0;
};
struct SubscriptionResponse {
    DeviceHandle subscription = {};
};
struct EventLinkReadResponse {
    bool enabled = false;
    bool hardware_autonomous = false;
    std::int64_t trigger_count = 0;
};
struct UsbDeviceDescription {
    std::uint8_t address = 0;
    std::uint16_t vendor_id = 0;
    std::uint16_t product_id = 0;
    ByteBufferPtr device_descriptor;
};
struct UsbDiscoveryResponse {
    std::vector<UsbDeviceDescription> devices;
};
struct UsbTransferResponse {
    ByteBufferPtr received;
    std::size_t bytes_transferred = 0;
};
struct UsbWriteResponse {
    std::size_t bytes_written = 0;
};

using DeviceResponse = std::variant<
    std::monostate, OpenResponse, EmptyResponse, AdcConfigureResponse, PwmConfigureResponse,
    SpiConfigureResponse, I2cConfigureResponse, UartConfigureResponse,
    PulseCaptureConfigureResponse, PulseOutputConfigureResponse, PcmConfigureResponse,
    UsbConfigureResponse, GpioReadResponse, AdcReadResponse, SpiTransferResponse,
    I2cTransferResponse, UartReadResponse, UartWriteResponse, PulseCaptureReadResponse,
    PulseOutputWriteResponse, PcmReadResponse, PcmWriteResponse, BleScanResponse,
    BleGattDiscoveryResponse, BleConnectionResponse, BleReadResponse, BleWriteResponse,
    SubscriptionResponse, EventLinkReadResponse, UsbDiscoveryResponse, UsbTransferResponse,
    UsbWriteResponse>;

struct BackendOperation {
    std::uint64_t owner_generation = 0;
    std::string_view logical_name;
    DeviceHandle device = {};
    DeviceHandle operation = {};
    // Non-empty only for subscribe. It is reserved by the session, not supplied
    // by the application or invented by the backend.
    DeviceHandle subscription = {};
    DeviceOperation id = DeviceOperation::none;
    DeviceBudget budget = {};
    DeviceRequest request = OpenRequest{};
};

// Implementations are internal native completion queues. They never contain a
// VM callback. complete() and publish() may be called by a driver worker or ISR
// adaptation; publish() applies event-queue and byte-budget backpressure.
class PeripheralCompletionSink {
public:
    virtual ~PeripheralCompletionSink() = default;
    virtual void complete(const BackendOperation& operation, IoStatus status,
                          DeviceResponse response) = 0;
    [[nodiscard]] virtual IoStatus publish(std::uint64_t owner_generation,
                                           DeviceHandle subscription,
                                           DeviceResponse response) = 0;
};

class PeripheralBackend {
public:
    virtual ~PeripheralBackend() = default;

    [[nodiscard]] virtual std::span<const DeviceOperation> operations() const noexcept = 0;
    // completed means accepted for asynchronous completion; the call itself
    // must only validate/enqueue native work. Any other status rejects the
    // submission, and in that case complete() must not be called. A backend
    // retaining work copies BackendOperation, never the reference itself.
    // Immutable buffer ownership then keeps application payload bytes alive.
    // DeviceSession revokes a close/unsubscribe resource only after this call
    // returns completed; a rejected control leaves that resource usable.
    virtual IoStatus start_operation(const BackendOperation& operation,
                                     std::weak_ptr<PeripheralCompletionSink> completions) = 0;
    // completed means cancellation was requested. The original operation must
    // subsequently complete with cancelled or its actual racing result.
    virtual IoStatus cancel_operation(const BackendOperation& operation) = 0;
    // Requests asynchronous stop/release of every lease for this application.
    // Return from this function does not assert that physical activity stopped.
    virtual void release_application(std::uint64_t owner_generation) = 0;
};

// Called by DeviceSession::poll() on its application thread. Implementations
// drain bounded queued results without blocking. std::shared_ptr and
// PeripheralCompletionSink remain in that thread for ports whose C++ library
// has no cross-thread shared-count support. The worker exchanges only
// kernel-synchronized plain data with this delivery function.
using PeripheralPoll = void (*)(PeripheralBackend& backend);
struct EndpointMetadata {
    EndpointDescriptor descriptor = {};
    std::span<const DeviceOperation> operations;

    [[nodiscard]] bool supports(DeviceOperation operation) const noexcept;
};

// Authorization is the intersection of signed-manifest grants and owner policy.
// Empty spans deny access; they never grant access to every endpoint.
struct ApplicationPermissions {
    std::span<const CapabilityVersion> capabilities;
    std::span<const std::string_view> endpoints;
};

class DeviceSession;

class PeripheralRegistry {
public:
    PeripheralRegistry();
    ~PeripheralRegistry();
    PeripheralRegistry(const PeripheralRegistry&) = delete;
    PeripheralRegistry& operator=(const PeripheralRegistry&) = delete;

    // Copies backend operations and the logical name. The backend must implement
    // every operation it returns; unsupported operations must not be listed.
    void register_endpoint(EndpointDescriptor descriptor, EndpointAccess access,
                           std::shared_ptr<PeripheralBackend> backend);
    // Additionally invokes poll_pending when a session created afterward
    // polls. The session retains the backend. Register deliveries before
    // creating sessions.
    void register_endpoint(EndpointDescriptor descriptor, EndpointAccess access,
                           std::shared_ptr<PeripheralBackend> backend,
                           PeripheralPoll poll_pending);
    [[nodiscard]] std::vector<EndpointMetadata> endpoints() const;
    [[nodiscard]] std::optional<EndpointMetadata> find_endpoint(std::string_view logical_name) const;
    [[nodiscard]] std::unique_ptr<DeviceSession> create_session(
        ApplicationPermissions permissions, DeviceBudget budget = {});

private:
    struct State;
    std::shared_ptr<State> m_state;
};

struct SubmissionResult {
    IoStatus status = IoStatus::invalid_argument;
    // Empty for a synchronous rejection. It identifies completion/cancellation;
    // it is not accepted in place of a device or subscription handle.
    DeviceHandle operation = {};
};

enum class DeviceResultKind : std::uint8_t { completion, notification };
struct DeviceResult {
    DeviceResultKind kind = DeviceResultKind::completion;
    DeviceHandle operation = {};
    DeviceHandle device = {};
    DeviceHandle subscription = {};
    DeviceOperation id = DeviceOperation::none;
    IoStatus status = IoStatus::invalid_argument;
    DeviceResponse response = std::monostate{};
};

class DeviceSession {
public:
    ~DeviceSession();
    DeviceSession(const DeviceSession&) = delete;
    DeviceSession& operator=(const DeviceSession&) = delete;

    [[nodiscard]] std::uint64_t owner_generation() const noexcept;
    [[nodiscard]] DeviceBudget budget() const noexcept;
    [[nodiscard]] std::vector<EndpointMetadata> endpoints() const;
    [[nodiscard]] SubmissionResult open(std::string_view logical_name,
                                        OpenRequest request = {});
    [[nodiscard]] SubmissionResult execute(DeviceHandle device, DeviceOperation operation,
                                           DeviceRequest request);
    [[nodiscard]] IoStatus cancel(DeviceHandle operation);
    // Calls every delivery function captured at session creation, then returns
    // at most one queued completion or notification. It never waits and never
    // executes application bytecode or an application handler.
    [[nodiscard]] std::optional<DeviceResult> poll();

    // Idempotently invalidates this application's handles and drops its queued
    // results. Each affected backend receives one asynchronous release request.
    void request_shutdown();

    [[nodiscard]] std::uint32_t pending_operation_count() const;
    [[nodiscard]] std::uint32_t open_handle_count() const;
    [[nodiscard]] std::uint32_t queued_notification_count() const;
    [[nodiscard]] std::uint64_t retained_byte_count() const;

private:
    friend class PeripheralRegistry;
    struct State;
    explicit DeviceSession(std::shared_ptr<State> state) noexcept;
    std::shared_ptr<State> m_state;
};

[[nodiscard]] ByteBufferPtr make_immutable_bytes(std::span<const std::uint8_t> bytes);

} // namespace mb
