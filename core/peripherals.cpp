#include "microbrowser/peripherals.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

#if defined(__ZEPHYR__) && !defined(_GLIBCXX_HAS_GTHREADS)
#include <zephyr/kernel.h>
#include <cerrno>
#endif

namespace mb {
namespace {
#if defined(__ZEPHYR__) && !defined(_GLIBCXX_HAS_GTHREADS)
// NCS's GNU C++ library has no std::mutex and single-threaded shared-count
// updates. NCS DeviceSession objects and callbacks stay on one Zephyr thread;
// worker and ISR boundaries carry only kernel-synchronized plain data.
class SessionMutex final {
public:
    SessionMutex() { k_mutex_init(&m_mutex); }
    SessionMutex(const SessionMutex&) = delete;
    SessionMutex& operator=(const SessionMutex&) = delete;

    void lock() {
        check_application_thread();
        if (k_mutex_lock(&m_mutex, K_FOREVER) != 0) k_oops();
    }

    [[nodiscard]] bool try_lock() {
        check_application_thread();
        const int result = k_mutex_lock(&m_mutex, K_NO_WAIT);
        if (result == 0) return true;
        if (result == -EBUSY) return false;
        k_oops();
        CODE_UNREACHABLE;
    }

    void unlock() {
        check_application_thread();
        if (k_mutex_unlock(&m_mutex) != 0) k_oops();
    }

private:
    void check_application_thread() {
        if (k_is_in_isr() || (m_owner != nullptr && k_current_get() != m_owner)) k_oops();
        if (m_owner == nullptr) m_owner = k_current_get();
    }

    k_mutex m_mutex{};
    k_thread* m_owner = nullptr;
};
#else
using SessionMutex = std::mutex;
#endif

struct EndpointDelivery {
    std::shared_ptr<PeripheralBackend> backend;
    PeripheralPoll poll_pending;
};


struct EndpointEntry {
    EndpointMetadata metadata;
    EndpointAccess access = EndpointAccess::owner_only;
    std::shared_ptr<PeripheralBackend> backend;
    std::string name;
    PeripheralPoll poll_pending = nullptr;
    std::vector<DeviceOperation> operations;
};

struct RegistryStorage {
    mutable SessionMutex mutex;
    std::vector<std::shared_ptr<EndpointEntry>> entries;
    std::uint64_t next_owner_generation = 0;
};

enum class SlotKind : std::uint8_t {
    empty,
    operation,
    opening_device,
    device,
    opening_subscription,
    subscription,
};

struct PendingOperation {
    BackendOperation backend;
    std::shared_ptr<EndpointEntry> endpoint;
    std::uint16_t resource_slot = 0;
    std::uint64_t accounted_bytes = 0;
    bool cancellation_requested = false;
    bool result_ready = false;
    DeviceResult result;
};

struct Slot {
    SlotKind kind = SlotKind::empty;
    Capability capability = Capability::none;
    std::uint32_t generation = 0;
    std::shared_ptr<EndpointEntry> endpoint;
    DeviceHandle owner_device = {};
    DeviceRequest subscription_request = OpenRequest{};
    std::optional<PendingOperation> pending;
};

struct QueuedNotification {
    DeviceResult result;
    std::uint64_t accounted_bytes = 0;
};

template <typename T>
class CompletionQueue {
public:
    explicit CompletionQueue(std::size_t capacity) : m_slots(capacity) {}

    [[nodiscard]] bool full() const noexcept { return m_count == m_slots.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_count == 0; }
    [[nodiscard]] std::size_t size() const noexcept { return m_count; }

    bool push(T value) {
        if (full()) return false;
        m_slots[(m_head + m_count) % m_slots.size()] = std::move(value);
        ++m_count;
        return true;
    }

    std::optional<T> pop() {
        if (empty()) return std::nullopt;
        T value = std::move(m_slots[m_head]);
        m_slots[m_head] = T{};
        m_head = (m_head + 1) % m_slots.size();
        --m_count;
        return value;
    }

    void clear() {
        while (!empty()) (void)pop();
    }

private:
    std::vector<T> m_slots;
    std::size_t m_head = 0;
    std::size_t m_count = 0;
};

[[nodiscard]] bool contains_operation(std::span<const DeviceOperation> operations,
                                      DeviceOperation operation) noexcept {
    return std::find(operations.begin(), operations.end(), operation) != operations.end();
}

[[nodiscard]] bool known_operation(DeviceOperation operation) noexcept {
    const auto value = static_cast<std::uint16_t>(operation);
    return value >= static_cast<std::uint16_t>(DeviceOperation::open) &&
           value <= static_cast<std::uint16_t>(DeviceOperation::advertise);
}

[[nodiscard]] bool valid_status(IoStatus status) noexcept {
    const auto value = static_cast<std::uint8_t>(status);
    return value <= static_cast<std::uint8_t>(IoStatus::cancelled);
}

void add_size(std::uint64_t& total, std::uint64_t amount) noexcept {
    if (amount > std::numeric_limits<std::uint64_t>::max() - total) {
        total = std::numeric_limits<std::uint64_t>::max();
    } else {
        total += amount;
    }
}

struct PayloadInspection {
    std::uint64_t bytes = 0;
    std::uint32_t maximum_bytes_per_buffer = 0;
    bool buffers_within_limit = true;

    void buffer(const ByteBufferPtr& buffer) {
        if (!buffer) return;
        add_size(bytes, buffer->size());
        buffers_within_limit = buffers_within_limit &&
                               buffer->size() <= maximum_bytes_per_buffer;
    }

    template <typename T>
    void sequence(const std::vector<T>& values) {
        if (values.capacity() > std::numeric_limits<std::uint64_t>::max() / sizeof(T)) {
            bytes = std::numeric_limits<std::uint64_t>::max();
        } else {
            add_size(bytes, values.capacity() * sizeof(T));
        }
    }
};

template <typename T>
void inspect_payload_value(const T& value, PayloadInspection& inspection) {
    if constexpr (requires { value.bytes; }) inspection.buffer(value.bytes);
    if constexpr (requires { value.transmit; }) inspection.buffer(value.transmit);
    if constexpr (requires { value.write_bytes; }) inspection.buffer(value.write_bytes);
    if constexpr (requires { value.frames; }) inspection.buffer(value.frames);
    if constexpr (requires { value.transmit_frames; }) inspection.buffer(value.transmit_frames);
    if constexpr (requires { value.value; }) inspection.buffer(value.value);
    if constexpr (requires { value.payload; }) inspection.buffer(value.payload);
    if constexpr (requires { value.received; }) inspection.buffer(value.received);
    if constexpr (requires { value.device_descriptor; }) inspection.buffer(value.device_descriptor);
    if constexpr (requires { value.descriptors; }) inspection.buffer(value.descriptors);
    if constexpr (requires { value.advertisement; }) inspection.buffer(value.advertisement);
    if constexpr (requires { value.durations_ns; }) {
        if (value.durations_ns) inspection.sequence(*value.durations_ns);
    }
    if constexpr (requires { value.interfaces; }) {
        inspection.sequence(value.interfaces);
        for (const auto& interface : value.interfaces) inspect_payload_value(interface, inspection);
    }
    if constexpr (requires { value.pulses; }) inspection.sequence(value.pulses);
    if constexpr (requires { value.peers; }) {
        inspection.sequence(value.peers);
        for (const auto& peer : value.peers) inspect_payload_value(peer, inspection);
    }
    if constexpr (requires { value.characteristics; }) inspection.sequence(value.characteristics);
    if constexpr (requires { value.devices; }) {
        inspection.sequence(value.devices);
        for (const auto& device : value.devices) inspect_payload_value(device, inspection);
    }
}

template <typename Variant>
[[nodiscard]] PayloadInspection inspect_payload(const Variant& payload,
                                                 std::uint32_t maximum_bytes_per_buffer) {
    PayloadInspection inspection{.maximum_bytes_per_buffer = maximum_bytes_per_buffer};
    std::visit([&](const auto& value) { inspect_payload_value(value, inspection); }, payload);
    return inspection;
}

[[nodiscard]] bool oversized_receive_request(const DeviceRequest& request,
                                              std::uint32_t maximum_bytes_per_buffer) {
    return std::visit(
        [&](const auto& value) {
            if constexpr (requires { value.byte_count; }) {
                if (value.byte_count > maximum_bytes_per_buffer) return true;
            }
            if constexpr (requires { value.maximum_bytes; }) {
                if (value.maximum_bytes > maximum_bytes_per_buffer) return true;
            }
            if constexpr (requires { value.receive_bytes; }) {
                if (value.receive_bytes > maximum_bytes_per_buffer) return true;
            }
            if constexpr (requires { value.read_bytes; }) {
                return value.read_bytes > maximum_bytes_per_buffer;
            }
            return false;
        },
        request);
}

template <typename... Types, typename Variant>
[[nodiscard]] bool holds(const Variant& value) {
    return (std::holds_alternative<Types>(value) || ...);
}

[[nodiscard]] bool configure_request_matches(Capability capability,
                                              const DeviceRequest& request) {
    switch (capability) {
    case Capability::gpio: return holds<GpioConfigureRequest>(request);
    case Capability::adc: return holds<AdcConfigureRequest>(request);
    case Capability::pwm: return holds<PwmConfigureRequest>(request);
    case Capability::pulse_capture: return holds<PulseCaptureConfigureRequest>(request);
    case Capability::pulse_output: return holds<PulseOutputConfigureRequest>(request);
    case Capability::spi_controller:
    case Capability::spi_peripheral:
    case Capability::spi_multilane: return holds<SpiConfigureRequest>(request);
    case Capability::i2c_controller:
    case Capability::i2c_peripheral: return holds<I2cConfigureRequest>(request);
    case Capability::uart:
    case Capability::rs485: return holds<UartConfigureRequest>(request);
    case Capability::i2s_transmit:
    case Capability::i2s_receive:
    case Capability::pdm_receive: return holds<PcmConfigureRequest>(request);
    case Capability::usb_device: return holds<UsbDeviceConfigureRequest>(request);
    default: return false;
    }
}

[[nodiscard]] bool read_request_matches(Capability capability, const DeviceRequest& request) {
    switch (capability) {
    case Capability::gpio: return holds<GpioReadRequest>(request);
    case Capability::adc: return holds<AdcReadRequest>(request);
    case Capability::pulse_capture: return holds<PulseCaptureReadRequest>(request);
    case Capability::spi_peripheral: return holds<SpiPeripheralReadRequest>(request);
    case Capability::i2c_controller: return holds<I2cReadRequest>(request);
    case Capability::i2c_peripheral: return holds<I2cPeripheralReadRequest>(request);
    case Capability::uart:
    case Capability::rs485: return holds<UartReadRequest>(request);
    case Capability::i2s_receive:
    case Capability::pdm_receive: return holds<PcmReadRequest>(request);
    case Capability::ble_central:
    case Capability::ble_peripheral: return holds<BleReadRequest>(request);
    case Capability::event_link: return holds<EventLinkReadRequest>(request);
    case Capability::usb_device:
    case Capability::usb_host: return holds<UsbReadRequest>(request);
    default: return false;
    }
}

[[nodiscard]] bool write_request_matches(Capability capability, const DeviceRequest& request) {
    switch (capability) {
    case Capability::gpio: return holds<GpioWriteRequest>(request);
    case Capability::pwm: return holds<PwmWriteRequest>(request);
    case Capability::pulse_output: return holds<PulseOutputWriteRequest>(request);
    case Capability::spi_peripheral: return holds<SpiPeripheralWriteRequest>(request);
    case Capability::i2c_controller: return holds<I2cWriteRequest>(request);
    case Capability::i2c_peripheral: return holds<I2cPeripheralWriteRequest>(request);
    case Capability::uart:
    case Capability::rs485: return holds<UartWriteRequest>(request);
    case Capability::i2s_transmit: return holds<PcmWriteRequest>(request);
    case Capability::ble_central:
    case Capability::ble_peripheral: return holds<BleWriteRequest>(request);
    case Capability::usb_device:
    case Capability::usb_host: return holds<UsbWriteRequest>(request);
    default: return false;
    }
}

[[nodiscard]] bool transfer_request_matches(Capability capability,
                                             const DeviceRequest& request) {
    switch (capability) {
    case Capability::spi_controller:
    case Capability::spi_peripheral:
    case Capability::spi_multilane: return holds<SpiTransferRequest>(request);
    case Capability::i2c_controller: return holds<I2cTransferRequest>(request);
    case Capability::i2s_transmit:
    case Capability::i2s_receive: return holds<PcmTransferRequest>(request);
    case Capability::usb_device:
    case Capability::usb_host: return holds<UsbTransferRequest>(request);
    default: return false;
    }
}

[[nodiscard]] bool subscribe_request_matches(Capability capability,
                                              const DeviceRequest& request) {
    switch (capability) {
    case Capability::gpio: return holds<GpioSubscribeRequest>(request);
    case Capability::ble_central:
    case Capability::ble_peripheral: return holds<BleSubscribeRequest>(request);
    default: return false;
    }
}

[[nodiscard]] bool request_matches(Capability capability, DeviceOperation operation,
                                   const DeviceRequest& request) {
    switch (operation) {
    case DeviceOperation::configure: return configure_request_matches(capability, request);
    case DeviceOperation::start: return holds<StartRequest>(request);
    case DeviceOperation::stop: return holds<StopRequest>(request);
    case DeviceOperation::close: return holds<CloseRequest>(request);
    case DeviceOperation::read: return read_request_matches(capability, request);
    case DeviceOperation::write: return write_request_matches(capability, request);
    case DeviceOperation::transfer: return transfer_request_matches(capability, request);
    case DeviceOperation::subscribe: return subscribe_request_matches(capability, request);
    case DeviceOperation::unsubscribe: return holds<UnsubscribeRequest>(request);
    case DeviceOperation::notify:
        return capability == Capability::ble_peripheral && holds<BleNotifyRequest>(request);
    case DeviceOperation::connect:
        if (capability == Capability::ble_central) return holds<BleConnectRequest>(request);
        return capability == Capability::usb_host && holds<UsbConnectRequest>(request);
    case DeviceOperation::disconnect:
        if (capability == Capability::ble_central) return holds<BleDisconnectRequest>(request);
        return capability == Capability::usb_host && holds<UsbDisconnectRequest>(request);
    case DeviceOperation::discover:
        if (capability == Capability::ble_central) return holds<BleDiscoverRequest>(request);
        return capability == Capability::usb_host && holds<UsbDiscoverRequest>(request);
    case DeviceOperation::advertise:
        return capability == Capability::ble_peripheral && holds<BleAdvertiseRequest>(request);
    default: return false;
    }
}

[[nodiscard]] bool response_matches(const BackendOperation& operation,
                                    const DeviceResponse& response) {
    if (std::holds_alternative<std::monostate>(response)) return false;
    const Capability capability = operation.device.capability == Capability::none
                                      ? operation.subscription.capability
                                      : operation.device.capability;
    const Capability effective_capability = operation.id == DeviceOperation::open
                                                 ? operation.operation.capability
                                                 : capability;
    switch (operation.id) {
    case DeviceOperation::open: return holds<OpenResponse>(response);
    case DeviceOperation::configure:
        switch (effective_capability) {
        case Capability::adc: return holds<AdcConfigureResponse>(response);
        case Capability::pwm: return holds<PwmConfigureResponse>(response);
        case Capability::spi_controller:
        case Capability::spi_peripheral:
        case Capability::spi_multilane: return holds<SpiConfigureResponse>(response);
        case Capability::i2c_controller:
        case Capability::i2c_peripheral: return holds<I2cConfigureResponse>(response);
        case Capability::uart:
        case Capability::rs485: return holds<UartConfigureResponse>(response);
        case Capability::pulse_capture: return holds<PulseCaptureConfigureResponse>(response);
        case Capability::pulse_output: return holds<PulseOutputConfigureResponse>(response);
        case Capability::i2s_transmit:
        case Capability::i2s_receive:
        case Capability::pdm_receive: return holds<PcmConfigureResponse>(response);
        case Capability::usb_device: return holds<UsbConfigureResponse>(response);
        default: return holds<EmptyResponse>(response);
        }
    case DeviceOperation::read:
        switch (effective_capability) {
        case Capability::gpio: return holds<GpioReadResponse>(response);
        case Capability::adc: return holds<AdcReadResponse>(response);
        case Capability::pulse_capture: return holds<PulseCaptureReadResponse>(response);
        case Capability::spi_controller:
        case Capability::spi_peripheral:
        case Capability::spi_multilane: return holds<SpiTransferResponse>(response);
        case Capability::i2c_controller:
        case Capability::i2c_peripheral: return holds<I2cTransferResponse>(response);
        case Capability::uart:
        case Capability::rs485: return holds<UartReadResponse>(response);
        case Capability::i2s_receive:
        case Capability::pdm_receive: return holds<PcmReadResponse>(response);
        case Capability::ble_central:
        case Capability::ble_peripheral: return holds<BleReadResponse>(response);
        case Capability::event_link: return holds<EventLinkReadResponse>(response);
        case Capability::usb_device:
        case Capability::usb_host: return holds<UsbTransferResponse>(response);
        default: return false;
        }
    case DeviceOperation::write:
        switch (effective_capability) {
        case Capability::i2c_controller:
        case Capability::i2c_peripheral: return holds<I2cTransferResponse>(response);
        case Capability::uart:
        case Capability::rs485: return holds<UartWriteResponse>(response);
        case Capability::pulse_output: return holds<PulseOutputWriteResponse>(response);
        case Capability::i2s_transmit: return holds<PcmWriteResponse>(response);
        case Capability::ble_central:
        case Capability::ble_peripheral: return holds<BleWriteResponse>(response);
        case Capability::usb_device:
        case Capability::usb_host: return holds<UsbWriteResponse>(response);
        default: return holds<EmptyResponse>(response);
        }
    case DeviceOperation::transfer:
        switch (effective_capability) {
        case Capability::spi_controller:
        case Capability::spi_peripheral:
        case Capability::spi_multilane: return holds<SpiTransferResponse>(response);
        case Capability::i2c_controller: return holds<I2cTransferResponse>(response);
        case Capability::i2s_transmit:
        case Capability::i2s_receive: return holds<PcmReadResponse>(response);
        case Capability::usb_device:
        case Capability::usb_host: return holds<UsbTransferResponse>(response);
        default: return false;
        }
    case DeviceOperation::subscribe: return holds<SubscriptionResponse>(response);
    case DeviceOperation::connect:
        return effective_capability == Capability::ble_central
                   ? holds<BleConnectionResponse>(response)
                   : holds<EmptyResponse>(response);
    case DeviceOperation::discover: {
        if (effective_capability == Capability::usb_host) {
            return holds<UsbDiscoveryResponse>(response);
        }
        const auto* discover = std::get_if<BleDiscoverRequest>(&operation.request);
        if (discover && discover->kind == BleDiscoveryKind::scan) {
            return holds<BleScanResponse>(response);
        }
        return holds<BleGattDiscoveryResponse>(response);
    }
    default: return holds<EmptyResponse>(response);
    }
}

[[nodiscard]] bool notification_matches(const Slot& slot, const DeviceResponse& response) {
    switch (slot.capability) {
    case Capability::gpio: return holds<GpioReadResponse>(response);
    case Capability::ble_central:
    case Capability::ble_peripheral: {
        const auto* subscription = std::get_if<BleSubscribeRequest>(&slot.subscription_request);
        if (subscription && subscription->target == BleSubscriptionTarget::connection_state) {
            return holds<BleConnectionResponse>(response);
        }
        return holds<BleReadResponse>(response);
    }
    default: return false;
    }
}

[[nodiscard]] bool version_supports(CapabilityVersion available,
                                    CapabilityVersion required) noexcept {
    return available.capability == required.capability &&
           available.major == peripheral_abi_major && available.major == required.major &&
           available.minor >= required.minor;
}

} // namespace

bool EndpointMetadata::supports(DeviceOperation operation) const noexcept {
    return contains_operation(operations, operation);
}

BleUuid BleUuid::from_uint16(std::uint16_t value) {
    BleUuid uuid;
    uuid.length = 2;
    uuid.bytes[0] = static_cast<std::uint8_t>(value);
    uuid.bytes[1] = static_cast<std::uint8_t>(value >> 8U);
    return uuid;
}

struct PeripheralRegistry::State : RegistryStorage {};

struct DeviceSession::State final : PeripheralCompletionSink,
                                    std::enable_shared_from_this<DeviceSession::State> {
    State(std::shared_ptr<RegistryStorage> registry_storage, std::uint64_t generation,
          ApplicationPermissions permissions, DeviceBudget budget)
        : registry(std::move(registry_storage)), owner_generation(generation), budget(budget),
          permissions(permissions.capabilities.begin(), permissions.capabilities.end()),
          allowed_endpoints(permissions.endpoints.begin(), permissions.endpoints.end()),
          slots(static_cast<std::size_t>(budget.maximum_open_handles) +
                static_cast<std::size_t>(budget.maximum_pending_operations)),
          completed_operations(budget.maximum_pending_operations),
          notifications(budget.maximum_pending_operations) {
        for (const auto& entry : this->registry->entries) {
            if (entry->poll_pending != nullptr) {
                deliveries.push_back({entry->backend, entry->poll_pending});
            }
        }
    }

    [[nodiscard]] std::shared_ptr<EndpointEntry> find(std::string_view name) const {
        std::lock_guard lock(registry->mutex);
        const auto found = std::find_if(registry->entries.begin(), registry->entries.end(),
                                        [&](const auto& entry) { return entry->name == name; });
        return found == registry->entries.end() ? nullptr : *found;
    }

    [[nodiscard]] bool capability_allowed(CapabilityVersion version) const noexcept {
        return std::any_of(permissions.begin(), permissions.end(), [&](CapabilityVersion grant) {
            return grant.capability == version.capability && grant.major == peripheral_abi_major &&
                   version.major == grant.major && grant.minor >= version.minor;
        });
    }

    [[nodiscard]] bool endpoint_name_allowed(const EndpointEntry& endpoint) const noexcept {
        return endpoint.access == EndpointAccess::application &&
               std::find(allowed_endpoints.begin(), allowed_endpoints.end(), endpoint.name) !=
                   allowed_endpoints.end();
    }

    [[nodiscard]] bool endpoint_allowed(const EndpointEntry& endpoint,
                                        CapabilityVersion minimum) const {
        if (!endpoint_name_allowed(endpoint)) return false;
        if (minimum.capability == Capability::none) {
            minimum = CapabilityVersion{endpoint.metadata.descriptor.version.capability,
                                        peripheral_abi_major, 0};
        }
        return capability_allowed(minimum);
    }

    [[nodiscard]] DeviceHandle handle_at(std::size_t index) const {
        const Slot& slot = slots[index];
        return DeviceHandle{slot.capability, static_cast<std::uint16_t>(index + 1),
                            slot.generation, owner_generation};
    }

    std::uint16_t allocate_slot(SlotKind kind, const std::shared_ptr<EndpointEntry>& endpoint) {
        for (std::size_t index = 0; index < slots.size(); ++index) {
            Slot& slot = slots[index];
            if (slot.kind != SlotKind::empty) continue;
            if (slot.generation == 0) slot.generation = 1;
            slot.kind = kind;
            slot.capability = endpoint->metadata.descriptor.version.capability;
            slot.endpoint = endpoint;
            slot.owner_device = {};
            slot.subscription_request = OpenRequest{};
            return static_cast<std::uint16_t>(index + 1);
        }
        return 0;
    }

    Slot* resolve(DeviceHandle handle, SlotKind expected) {
        if (handle.owner_generation != owner_generation || handle.slot == 0 ||
            handle.slot > slots.size()) {
            return nullptr;
        }
        Slot& slot = slots[handle.slot - 1];
        if (slot.kind != expected || slot.generation != handle.object_generation ||
            slot.capability != handle.capability || handle.object_generation == 0) {
            return nullptr;
        }
        return &slot;
    }

    [[nodiscard]] const Slot* resolve(DeviceHandle handle, SlotKind expected) const {
        return const_cast<State*>(this)->resolve(handle, expected);
    }

    void release_slot(std::uint16_t index) {
        if (index == 0) return;
        Slot& slot = slots[index - 1];
        if (slot.kind == SlotKind::operation) {
            if (slot.pending) {
                retained_bytes -= slot.pending->accounted_bytes;
                --operation_count;
            }
        } else if (slot.kind != SlotKind::empty) {
            --resource_count;
        }
        slot.pending.reset();
        slot.endpoint.reset();
        slot.owner_device = {};
        slot.subscription_request = OpenRequest{};
        slot.capability = Capability::none;
        slot.kind = SlotKind::empty;
        if (slot.generation == std::numeric_limits<std::uint32_t>::max()) {
            slot.generation = 1;
        } else {
            ++slot.generation;
        }
    }

    [[nodiscard]] IoStatus reserve_payload(const DeviceRequest& request,
                                           PayloadInspection& inspection) {
        inspection = inspect_payload(request, budget.maximum_bytes_per_buffer);
        if (oversized_receive_request(request, budget.maximum_bytes_per_buffer)) {
            return IoStatus::memory_exhausted;
        }
        if (!inspection.buffers_within_limit) return IoStatus::memory_exhausted;
        const std::uint64_t available =
            static_cast<std::uint64_t>(budget.maximum_retained_bytes) -
            std::min<std::uint64_t>(retained_bytes, budget.maximum_retained_bytes);
        return inspection.bytes <= available ? IoStatus::completed : IoStatus::memory_exhausted;
    }

    [[nodiscard]] SubmissionResult submit_open(const std::shared_ptr<EndpointEntry>& endpoint,
                                                OpenRequest request) {
        CapabilityVersion minimum = request.minimum_version;
        if (!endpoint_name_allowed(*endpoint)) return {IoStatus::permission_denied, {}};
        if (minimum.capability == Capability::none ||
            minimum.capability != endpoint->metadata.descriptor.version.capability) {
            return {IoStatus::invalid_argument, {}};
        }
        if (!version_supports(endpoint->metadata.descriptor.version, minimum)) {
            return {IoStatus::unsupported, {}};
        }
        if (!endpoint_allowed(*endpoint, minimum)) return {IoStatus::permission_denied, {}};
        if (!endpoint->metadata.supports(DeviceOperation::open)) {
            return {IoStatus::unsupported, {}};
        }

        BackendOperation backend;
        backend.owner_generation = owner_generation;
        backend.logical_name = endpoint->name;
        backend.id = DeviceOperation::open;
        backend.budget = budget;
        backend.request = request;
        return start_submission(endpoint, std::move(backend));
    }

    [[nodiscard]] SubmissionResult submit(DeviceHandle device, DeviceOperation operation,
                                          DeviceRequest request) {
        std::unique_lock lock(mutex);
        if (!active) return {IoStatus::device_lost, {}};
        if (!known_operation(operation)) return {IoStatus::unsupported, {}};
        Slot* device_slot = resolve(device, SlotKind::device);
        if (!device_slot) return {IoStatus::invalid_argument, {}};
        auto endpoint = device_slot->endpoint;
        if (!endpoint_allowed(*endpoint, CapabilityVersion{})) {
            return {IoStatus::permission_denied, {}};
        }
        if (operation == DeviceOperation::cancel) return {IoStatus::invalid_argument, {}};
        if (!endpoint->metadata.supports(operation)) return {IoStatus::unsupported, {}};
        if (!request_matches(device.capability, operation, request)) {
            return {IoStatus::invalid_argument, {}};
        }
        if (operation == DeviceOperation::unsubscribe) {
            const auto& unsubscribe = std::get<UnsubscribeRequest>(request);
            const Slot* subscription = resolve(unsubscribe.subscription, SlotKind::subscription);
            if (!subscription || subscription->owner_device != device) {
                return {IoStatus::invalid_argument, {}};
            }
        }

        BackendOperation backend;
        backend.owner_generation = owner_generation;
        backend.logical_name = endpoint->name;
        backend.device = device;
        backend.id = operation;
        backend.budget = budget;
        backend.request = std::move(request);
        return start_submission_locked(endpoint, std::move(backend), lock);
    }

    [[nodiscard]] SubmissionResult start_submission(const std::shared_ptr<EndpointEntry>& endpoint,
                                                     BackendOperation backend) {
        std::unique_lock lock(mutex);
        if (!active) return {IoStatus::device_lost, {}};
        return start_submission_locked(endpoint, std::move(backend), lock);
    }

    [[nodiscard]] SubmissionResult start_submission_locked(
        const std::shared_ptr<EndpointEntry>& endpoint, BackendOperation backend,
        std::unique_lock<SessionMutex>& lock) {
        PayloadInspection inspection;
        const IoStatus payload_status = reserve_payload(backend.request, inspection);
        if (payload_status != IoStatus::completed) return {payload_status, {}};
        if (operation_count >= budget.maximum_pending_operations) return {IoStatus::busy, {}};

        const bool needs_device = backend.id == DeviceOperation::open;
        const bool needs_subscription = backend.id == DeviceOperation::subscribe;
        if ((needs_device || needs_subscription) && resource_count >= budget.maximum_open_handles) {
            return {IoStatus::busy, {}};
        }

        const std::uint16_t operation_slot = allocate_slot(SlotKind::operation, endpoint);
        if (operation_slot == 0) return {IoStatus::busy, {}};
        ++operation_count;
        backend.operation = handle_at(operation_slot - 1);

        std::uint16_t resource_slot = 0;
        if (needs_device || needs_subscription) {
            resource_slot = allocate_slot(needs_device ? SlotKind::opening_device
                                                       : SlotKind::opening_subscription,
                                          endpoint);
            if (resource_slot == 0) {
                release_slot(operation_slot);
                return {IoStatus::busy, {}};
            }
            ++resource_count;
            const DeviceHandle resource = handle_at(resource_slot - 1);
            if (needs_subscription) {
                backend.subscription = resource;
                slots[resource_slot - 1].owner_device = backend.device;
                slots[resource_slot - 1].subscription_request = backend.request;
            }
        }

        Slot& operation = slots[operation_slot - 1];
        operation.pending.emplace(PendingOperation{
            .backend = std::move(backend),
            .endpoint = endpoint,
            .resource_slot = resource_slot,
            .accounted_bytes = inspection.bytes,
            .result = {},
        });
        retained_bytes += inspection.bytes;
        const DeviceHandle operation_handle = operation.pending->backend.operation;
        const DeviceOperation control = operation.pending->backend.id;
        const DeviceHandle control_resource =
            control == DeviceOperation::unsubscribe
                ? std::get<UnsubscribeRequest>(operation.pending->backend.request).subscription
                : operation.pending->backend.device;



        const auto driver = operation.pending->endpoint->backend;
        const BackendOperation driver_operation = operation.pending->backend;
        auto sink = std::static_pointer_cast<PeripheralCompletionSink>(shared_from_this());
        lock.unlock();
        const IoStatus status = driver->start_operation(driver_operation, std::move(sink));
        lock.lock();
        if (valid_status(status) && status == IoStatus::completed) {
            if (active) {
                if (control == DeviceOperation::close) {
                    invalidate_device(control_resource, operation_handle);
                } else if (control == DeviceOperation::unsubscribe) {
                    release_slot(control_resource.slot);
                }
            }
            return {IoStatus::completed, operation_handle};
        }
        reject_submission(operation_handle);
        return {valid_status(status) ? status : IoStatus::io_failure, {}};
    }

    void reject_submission(DeviceHandle operation_handle) {
        Slot* operation = resolve(operation_handle, SlotKind::operation);
        if (!operation || !operation->pending || operation->pending->result_ready) return;
        const std::uint16_t resource_slot = operation->pending->resource_slot;
        if (resource_slot != 0) {
            Slot& resource = slots[resource_slot - 1];
            if (resource.kind == SlotKind::opening_device ||
                resource.kind == SlotKind::opening_subscription) {
                release_slot(resource_slot);
            }
        }
        release_slot(operation_handle.slot);
    }

    void invalidate_device(DeviceHandle device, DeviceHandle close_operation) {
        for (auto& slot : slots) {
            if ((slot.kind == SlotKind::subscription ||
                 slot.kind == SlotKind::opening_subscription) &&
                slot.owner_device == device) {
                release_slot(static_cast<std::uint16_t>(&slot - slots.data() + 1));
            }
        }
        for (auto& slot : slots) {
            if (slot.kind != SlotKind::operation || !slot.pending || slot.pending->result_ready) {
                continue;
            }
            const DeviceHandle handle = slot.pending->backend.operation;
            if (handle == close_operation || slot.pending->backend.device != device) continue;
            finish_operation(slot, IoStatus::cancelled, EmptyResponse{});
        }
        release_slot(device.slot);
    }

    SubmissionResult execute_on_device(DeviceHandle device, DeviceOperation operation,
                                       DeviceRequest request) {
        return submit(device, operation, std::move(request));
    }

    void finish_operation(Slot& operation_slot, IoStatus status, DeviceResponse response) {
        PendingOperation& pending = *operation_slot.pending;
        if (pending.result_ready || !active) return;
        if (!valid_status(status)) status = IoStatus::io_failure;
        if (status == IoStatus::completed && !response_matches(pending.backend, response)) {
            status = IoStatus::io_failure;
            response = std::monostate{};
        }

        if (pending.backend.id == DeviceOperation::open) {
            auto* open = std::get_if<OpenResponse>(&response);
            if (status == IoStatus::completed && (!open || !open->device.is_empty())) {
                status = IoStatus::io_failure;
                response = std::monostate{};
            }
        } else if (pending.backend.id == DeviceOperation::subscribe) {
            auto* subscription = std::get_if<SubscriptionResponse>(&response);
            if (status == IoStatus::completed &&
                (!subscription || (!subscription->subscription.is_empty() &&
                                   subscription->subscription != pending.backend.subscription))) {
                status = IoStatus::io_failure;
                response = std::monostate{};
            }
        }

        PayloadInspection inspection = inspect_payload(response, budget.maximum_bytes_per_buffer);
        const std::uint64_t available_bytes =
            static_cast<std::uint64_t>(budget.maximum_retained_bytes) -
            std::min<std::uint64_t>(retained_bytes - pending.accounted_bytes,
                                    budget.maximum_retained_bytes);
        if (!inspection.buffers_within_limit || inspection.bytes > available_bytes) {
            status = IoStatus::memory_exhausted;
            response = std::monostate{};
            inspection = {};
        }

        if (pending.accounted_bytes <= retained_bytes) retained_bytes -= pending.accounted_bytes;
        pending.accounted_bytes = inspection.bytes;
        retained_bytes += inspection.bytes;

        DeviceResult result;
        result.kind = DeviceResultKind::completion;
        result.operation = pending.backend.operation;
        result.device = pending.backend.device;
        result.subscription = pending.backend.subscription;
        result.id = pending.backend.id;
        result.status = status;

        const bool opens_resource = pending.backend.id == DeviceOperation::open ||
                                    pending.backend.id == DeviceOperation::subscribe;
        if (opens_resource && status != IoStatus::completed) {
            release_slot(pending.resource_slot);
            pending.resource_slot = 0;
            response = std::monostate{};
            if (pending.accounted_bytes != 0) {
                retained_bytes -= pending.accounted_bytes;
                pending.accounted_bytes = 0;
            }
        } else if (pending.backend.id == DeviceOperation::open) {
            Slot& resource = slots[pending.resource_slot - 1];
            resource.kind = SlotKind::device;
            const DeviceHandle device = handle_at(pending.resource_slot - 1);
            response = OpenResponse{device, resource.endpoint->metadata.descriptor.version,
                                    resource.endpoint->metadata.descriptor.modeled};
            result.device = device;
        } else if (pending.backend.id == DeviceOperation::subscribe) {
            Slot& resource = slots[pending.resource_slot - 1];
            resource.kind = SlotKind::subscription;
            const DeviceHandle subscription = handle_at(pending.resource_slot - 1);
            response = SubscriptionResponse{subscription};
            result.subscription = subscription;
        }

        result.response = std::move(response);
        pending.result = std::move(result);
        pending.result_ready = true;
        // Payload matching for discovery needs the request until this point.
        // Releasing it here prevents completed work retaining application bytes.
        pending.backend.request = OpenRequest{};
        const bool pushed = completed_operations.push(operation_slot.pending->backend.operation.slot);
        if (!pushed) std::terminate();
    }

    void complete(const BackendOperation& backend, IoStatus status,
                  DeviceResponse response) override {
        std::lock_guard lock(mutex);
        if (!active || backend.owner_generation != owner_generation) return;
        Slot* operation = resolve(backend.operation, SlotKind::operation);
        if (!operation || !operation->pending || operation->pending->result_ready) return;
        finish_operation(*operation, status, std::move(response));
    }

    IoStatus publish(std::uint64_t generation, DeviceHandle subscription,
                     DeviceResponse response) override {
        std::lock_guard lock(mutex);
        if (!active || generation != owner_generation) return IoStatus::device_lost;
        Slot* subscription_slot = resolve(subscription, SlotKind::subscription);
        if (!subscription_slot) return IoStatus::invalid_argument;
        if (notifications.full()) return IoStatus::busy;
        if (!notification_matches(*subscription_slot, response)) return IoStatus::io_failure;

        PayloadInspection inspection = inspect_payload(response, budget.maximum_bytes_per_buffer);
        const std::uint64_t available_bytes =
            static_cast<std::uint64_t>(budget.maximum_retained_bytes) -
            std::min<std::uint64_t>(retained_bytes, budget.maximum_retained_bytes);
        if (!inspection.buffers_within_limit || inspection.bytes > available_bytes) {
            return IoStatus::memory_exhausted;
        }

        DeviceResult result;
        result.kind = DeviceResultKind::notification;
        result.device = subscription_slot->owner_device;
        result.subscription = subscription;
        result.id = DeviceOperation::notify;
        result.status = IoStatus::completed;
        result.response = std::move(response);
        const bool pushed = notifications.push({std::move(result), inspection.bytes});
        if (!pushed) return IoStatus::busy;
        retained_bytes += inspection.bytes;
        return IoStatus::completed;
    }

    IoStatus cancel(DeviceHandle operation_handle) {
        BackendOperation backend;
        std::shared_ptr<PeripheralBackend> driver;
        {
            std::lock_guard lock(mutex);
            if (!active) return IoStatus::device_lost;
            Slot* operation = resolve(operation_handle, SlotKind::operation);
            if (!operation || !operation->pending || operation->pending->result_ready ||
                operation->pending->cancellation_requested) {
                return IoStatus::invalid_argument;
            }
            if (!operation->pending->endpoint->metadata.supports(DeviceOperation::cancel)) {
                return IoStatus::unsupported;
            }
            operation->pending->cancellation_requested = true;
            backend = operation->pending->backend;
            driver = operation->pending->endpoint->backend;
        }
        const IoStatus status = driver->cancel_operation(backend);
        if (status == IoStatus::completed) return status;
        std::lock_guard lock(mutex);
        Slot* operation = resolve(operation_handle, SlotKind::operation);
        if (operation && operation->pending && !operation->pending->result_ready) {
            operation->pending->cancellation_requested = false;
        }
        return valid_status(status) ? status : IoStatus::io_failure;
    }

    std::optional<DeviceResult> poll() {
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock() || !active) return std::nullopt;
        lock.unlock();
        for (const EndpointDelivery& delivery : deliveries) {
            delivery.poll_pending(*delivery.backend);
        }
        lock.lock();
        if (!active) return std::nullopt;
        if (auto completed = completed_operations.pop()) {
            Slot& operation = slots[*completed - 1];
            if (operation.kind != SlotKind::operation || !operation.pending ||
                !operation.pending->result_ready) {
                std::terminate();
            }
            DeviceResult result = std::move(operation.pending->result);
            release_slot(*completed);
            return result;
        }
        if (auto notification = notifications.pop()) {
            retained_bytes -= notification->accounted_bytes;
            return std::move(notification->result);
        }
        return std::nullopt;
    }

    void request_shutdown() {
        std::vector<std::shared_ptr<PeripheralBackend>> drivers;
        {
            std::lock_guard lock(mutex);
            if (!active) return;
            active = false;
            for (Slot& slot : slots) {
                if (slot.endpoint &&
                    std::find(drivers.begin(), drivers.end(), slot.endpoint->backend) == drivers.end()) {
                    drivers.push_back(slot.endpoint->backend);
                }
                slot.pending.reset();
                slot.endpoint.reset();
                slot.owner_device = {};
                slot.subscription_request = OpenRequest{};
                slot.capability = Capability::none;
                slot.kind = SlotKind::empty;
                if (slot.generation == std::numeric_limits<std::uint32_t>::max()) {
                    slot.generation = 1;
                } else {
                    ++slot.generation;
                }
            }
            completed_operations.clear();
            notifications.clear();
            retained_bytes = 0;
            operation_count = 0;
            resource_count = 0;
        }
        for (const auto& driver : drivers) driver->release_application(owner_generation);
    }

    std::vector<EndpointMetadata> visible_endpoints() const {
        if (!active) return {};
        std::lock_guard registry_lock(registry->mutex);
        std::vector<EndpointMetadata> result;
        for (const auto& entry : registry->entries) {
            if (entry->access == EndpointAccess::application && endpoint_allowed(*entry, {})) {
                result.push_back(entry->metadata);
            }
        }
        return result;
    }

    std::shared_ptr<RegistryStorage> registry;
    const std::uint64_t owner_generation;
    const DeviceBudget budget;
    std::vector<CapabilityVersion> permissions;
    std::vector<std::string> allowed_endpoints;

    std::vector<EndpointDelivery> deliveries;
    mutable SessionMutex mutex;
    bool active = true;
    std::vector<Slot> slots;
    CompletionQueue<std::uint16_t> completed_operations;
    CompletionQueue<QueuedNotification> notifications;
    std::uint32_t operation_count = 0;
    std::uint32_t resource_count = 0;
    std::uint64_t retained_bytes = 0;
};

PeripheralRegistry::PeripheralRegistry() : m_state(std::make_shared<State>()) {}
PeripheralRegistry::~PeripheralRegistry() = default;

void PeripheralRegistry::register_endpoint(EndpointDescriptor descriptor, EndpointAccess access,
                                           std::shared_ptr<PeripheralBackend> backend) {
    register_endpoint(std::move(descriptor), access, std::move(backend), nullptr);
}

void PeripheralRegistry::register_endpoint(EndpointDescriptor descriptor, EndpointAccess access,
                                           std::shared_ptr<PeripheralBackend> backend,
                                           PeripheralPoll poll_pending) {
    if (!backend || descriptor.logical_name.empty() ||
        descriptor.version.capability == Capability::none ||
        descriptor.version.major != peripheral_abi_major) {
        throw std::invalid_argument("invalid peripheral endpoint registration");
    }
    auto entry = std::make_shared<EndpointEntry>();
    entry->name = descriptor.logical_name;
    entry->poll_pending = poll_pending;
    entry->access = access;
    entry->backend = std::move(backend);
    const std::span<const DeviceOperation> implemented = entry->backend->operations();
    entry->operations.assign(implemented.begin(), implemented.end());
    if (entry->operations.empty()) {
        throw std::invalid_argument("peripheral endpoint has no implemented operations");
    }
    for (std::size_t index = 0; index < entry->operations.size(); ++index) {
        if (!known_operation(entry->operations[index]) ||
            std::find(entry->operations.begin(), entry->operations.begin() + index,
                      entry->operations[index]) != entry->operations.begin() + index) {
            throw std::invalid_argument("unknown or duplicate peripheral operation");
        }
    }
    entry->metadata = EndpointMetadata{
        EndpointDescriptor{entry->name, descriptor.version, descriptor.modeled}, entry->operations};

    std::lock_guard lock(m_state->mutex);
    const auto duplicate = std::find_if(m_state->entries.begin(), m_state->entries.end(),
                                        [&](const auto& existing) {
                                            return existing->name == entry->name;
                                        });
    if (duplicate != m_state->entries.end()) {
        throw std::invalid_argument("duplicate logical peripheral endpoint");
    }
    m_state->entries.push_back(std::move(entry));
}

std::vector<EndpointMetadata> PeripheralRegistry::endpoints() const {
    std::lock_guard lock(m_state->mutex);
    std::vector<EndpointMetadata> result;
    result.reserve(m_state->entries.size());
    for (const auto& entry : m_state->entries) result.push_back(entry->metadata);
    return result;
}

std::optional<EndpointMetadata> PeripheralRegistry::find_endpoint(
    std::string_view logical_name) const {
    std::lock_guard lock(m_state->mutex);
    const auto found = std::find_if(m_state->entries.begin(), m_state->entries.end(),
                                    [&](const auto& entry) {
                                        return entry->name == logical_name;
                                    });
    if (found == m_state->entries.end()) return std::nullopt;
    return (*found)->metadata;
}

std::unique_ptr<DeviceSession> PeripheralRegistry::create_session(
    ApplicationPermissions permissions, DeviceBudget budget) {
    const std::uint64_t total_slots = static_cast<std::uint64_t>(budget.maximum_open_handles) +
                                      budget.maximum_pending_operations;
    if (total_slots > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("peripheral handle budget exceeds 16-bit slot identifiers");
    }
    std::shared_ptr<State> registry = m_state;
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(registry->mutex);
        if (registry->next_owner_generation == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("application owner generation exhausted");
        }
        generation = ++registry->next_owner_generation;
    }
    auto state = std::make_shared<DeviceSession::State>(
        std::static_pointer_cast<RegistryStorage>(registry), generation, permissions, budget);
    return std::unique_ptr<DeviceSession>(new DeviceSession(std::move(state)));
}

DeviceSession::DeviceSession(std::shared_ptr<State> state) noexcept : m_state(std::move(state)) {}
DeviceSession::~DeviceSession() { request_shutdown(); }

std::uint64_t DeviceSession::owner_generation() const noexcept { return m_state->owner_generation; }

DeviceBudget DeviceSession::budget() const noexcept { return m_state->budget; }

std::vector<EndpointMetadata> DeviceSession::endpoints() const {
    std::lock_guard lock(m_state->mutex);
    return m_state->visible_endpoints();
}

SubmissionResult DeviceSession::open(std::string_view logical_name, OpenRequest request) {
    auto endpoint = m_state->find(logical_name);
    if (!endpoint) return {IoStatus::invalid_argument, {}};
    return m_state->submit_open(endpoint, request);
}

SubmissionResult DeviceSession::execute(DeviceHandle device, DeviceOperation operation,
                                        DeviceRequest request) {
    return m_state->execute_on_device(device, operation, std::move(request));
}

IoStatus DeviceSession::cancel(DeviceHandle operation) { return m_state->cancel(operation); }
std::optional<DeviceResult> DeviceSession::poll() { return m_state->poll(); }
void DeviceSession::request_shutdown() { m_state->request_shutdown(); }

std::uint32_t DeviceSession::pending_operation_count() const {
    std::lock_guard lock(m_state->mutex);
    return m_state->operation_count;
}
std::uint32_t DeviceSession::open_handle_count() const {
    std::lock_guard lock(m_state->mutex);
    return m_state->resource_count;
}
std::uint32_t DeviceSession::queued_notification_count() const {
    std::lock_guard lock(m_state->mutex);
    return static_cast<std::uint32_t>(m_state->notifications.size());
}
std::uint64_t DeviceSession::retained_byte_count() const {
    std::lock_guard lock(m_state->mutex);
    return m_state->retained_bytes;
}

ByteBufferPtr make_immutable_bytes(std::span<const std::uint8_t> bytes) {
    return std::make_shared<const ByteBuffer>(bytes.begin(), bytes.end());
}

} // namespace mb
