#include "platform/zephyr_ble_central.h"

#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mb::zephyr {
namespace {

constexpr std::uint8_t kNoIndex = std::numeric_limits<std::uint8_t>::max();
constexpr std::uint32_t kMinimumSupervisionTimeoutMs = 100;
constexpr std::uint32_t kMaximumSupervisionTimeoutMs = 32'000;

constexpr std::array<DeviceOperation, 10> kImplementedOperations{
    DeviceOperation::open,
    DeviceOperation::discover,
    DeviceOperation::connect,
    DeviceOperation::disconnect,
    DeviceOperation::read,
    DeviceOperation::write,
    DeviceOperation::subscribe,
    DeviceOperation::unsubscribe,
    DeviceOperation::close,
    DeviceOperation::cancel,
};

const bt_uuid_16 kPrimaryServiceUuid = BT_UUID_INIT_16(BT_UUID_GATT_PRIMARY_VAL);
const bt_uuid_16 kCharacteristicUuid = BT_UUID_INIT_16(BT_UUID_GATT_CHRC_VAL);
const bt_uuid_16 kCccdUuid = BT_UUID_INIT_16(BT_UUID_GATT_CCC_VAL);

[[nodiscard]] bool same_uuid(const BleUuid& left, const BleUuid& right) noexcept {
    return left.length == right.length && left.length <= left.bytes.size() &&
           std::equal(left.bytes.begin(), left.bytes.begin() + left.length,
                      right.bytes.begin());
}

[[nodiscard]] bool same_address(const BleAddress& left, const BleAddress& right) noexcept {
    return left.random == right.random && left.bytes == right.bytes;
}

[[nodiscard]] bool same_attribute(const BleAttribute& left,
                                  const BleAttribute& right) noexcept {
    return same_uuid(left.service, right.service) &&
           same_uuid(left.characteristic, right.characteristic);
}

[[nodiscard]] bool wildcard_uuid(const BleUuid& uuid) noexcept {
    return uuid.length == 0 &&
           std::all_of(uuid.bytes.begin(), uuid.bytes.end(),
                       [](std::uint8_t byte) { return byte == 0; });
}

[[nodiscard]] bool matches_uuid_filter(const BleUuid& uuid, const BleUuid& filter) noexcept {
    return wildcard_uuid(filter) || same_uuid(uuid, filter);
}

[[nodiscard]] bool valid_uuid(const BleUuid& uuid, bool wildcard_allowed = false) noexcept {
    if (wildcard_uuid(uuid)) return wildcard_allowed;
    if (uuid.length != 2 && uuid.length != 16) return false;
    return std::all_of(uuid.bytes.begin() + uuid.length, uuid.bytes.end(),
                       [](std::uint8_t byte) { return byte == 0; });
}

[[nodiscard]] bool valid_attribute(const BleAttribute& attribute,
                                   bool wildcard_allowed = false) noexcept {
    return valid_uuid(attribute.service, wildcard_allowed) &&
           valid_uuid(attribute.characteristic, wildcard_allowed);
}

[[nodiscard]] bool wildcard_attribute(const BleAttribute& attribute) noexcept {
    return wildcard_uuid(attribute.service) && wildcard_uuid(attribute.characteristic);
}

[[nodiscard]] bool request_matches(const BackendOperation& operation) noexcept {
    switch (operation.id) {
    case DeviceOperation::open:
        return std::holds_alternative<OpenRequest>(operation.request);
    case DeviceOperation::discover:
        return std::holds_alternative<BleDiscoverRequest>(operation.request);
    case DeviceOperation::connect:
        return std::holds_alternative<BleConnectRequest>(operation.request);
    case DeviceOperation::disconnect:
        return std::holds_alternative<BleDisconnectRequest>(operation.request);
    case DeviceOperation::read:
        return std::holds_alternative<BleReadRequest>(operation.request);
    case DeviceOperation::write:
        return std::holds_alternative<BleWriteRequest>(operation.request);
    case DeviceOperation::subscribe:
        return std::holds_alternative<BleSubscribeRequest>(operation.request);
    case DeviceOperation::unsubscribe:
        return std::holds_alternative<UnsubscribeRequest>(operation.request);
    case DeviceOperation::close:
        return std::holds_alternative<CloseRequest>(operation.request);
    default:
        return false;
    }
}

[[nodiscard]] IoStatus errno_status(int error) noexcept {
    if (error >= 0) return IoStatus::completed;
    switch (-error) {
    case ECANCELED: return IoStatus::cancelled;
    case EINVAL: return IoStatus::invalid_argument;
    case EACCES:
    case EPERM: return IoStatus::permission_denied;
    case ENOTSUP:
    case ENOSYS: return IoStatus::unsupported;
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
    case EOPNOTSUPP: return IoStatus::unsupported;
#endif
    case EBUSY:
    case EALREADY:
    case EAGAIN: return IoStatus::busy;
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK: return IoStatus::busy;
#endif
    case ETIMEDOUT: return IoStatus::timeout;
    case ENODEV:
    case ENXIO:
    case ENOTCONN: return IoStatus::device_lost;
    case ENOMEM: return IoStatus::memory_exhausted;
    default: return IoStatus::io_failure;
    }
}

[[nodiscard]] IoStatus att_status(std::uint8_t error) noexcept {
    switch (error) {
    case BT_ATT_ERR_SUCCESS: return IoStatus::completed;
    case BT_ATT_ERR_INVALID_HANDLE:
    case BT_ATT_ERR_INVALID_PDU:
    case BT_ATT_ERR_INVALID_OFFSET:
    case BT_ATT_ERR_ATTRIBUTE_NOT_FOUND:
    case BT_ATT_ERR_INVALID_ATTRIBUTE_LEN:
    case BT_ATT_ERR_VALUE_NOT_ALLOWED:
    case BT_ATT_ERR_OUT_OF_RANGE: return IoStatus::invalid_argument;
    case BT_ATT_ERR_AUTHENTICATION:
    case BT_ATT_ERR_AUTHORIZATION:
    case BT_ATT_ERR_ENCRYPTION_KEY_SIZE:
    case BT_ATT_ERR_INSUFFICIENT_ENCRYPTION:
    case BT_ATT_ERR_WRITE_REQ_REJECTED:
    case BT_ATT_ERR_CCC_IMPROPER_CONF: return IoStatus::permission_denied;
    case BT_ATT_ERR_READ_NOT_PERMITTED:
    case BT_ATT_ERR_WRITE_NOT_PERMITTED:
    case BT_ATT_ERR_NOT_SUPPORTED:
    case BT_ATT_ERR_ATTRIBUTE_NOT_LONG:
    case BT_ATT_ERR_UNSUPPORTED_GROUP_TYPE: return IoStatus::unsupported;
    case BT_ATT_ERR_PREPARE_QUEUE_FULL:
    case BT_ATT_ERR_INSUFFICIENT_RESOURCES: return IoStatus::memory_exhausted;
    case BT_ATT_ERR_PROCEDURE_IN_PROGRESS: return IoStatus::busy;
    default: return IoStatus::io_failure;
    }
}

[[nodiscard]] IoStatus hci_status(std::uint8_t error) noexcept {
    switch (error) {
    case BT_HCI_ERR_SUCCESS: return IoStatus::completed;
    case BT_HCI_ERR_CONN_TIMEOUT:
    case BT_HCI_ERR_ADV_TIMEOUT: return IoStatus::timeout;
    case BT_HCI_ERR_INSUFFICIENT_RESOURCES:
    case BT_HCI_ERR_LIMIT_REACHED: return IoStatus::memory_exhausted;
    case BT_HCI_ERR_AUTH_FAIL:
    case BT_HCI_ERR_INSUFFICIENT_SECURITY:
    case BT_HCI_ERR_PAIRING_NOT_ALLOWED:
    case BT_HCI_ERR_PAIRING_NOT_SUPPORTED: return IoStatus::permission_denied;
    case BT_HCI_ERR_UNSUPP_FEATURE_PARAM_VAL:
    case BT_HCI_ERR_UNSUPP_REMOTE_FEATURE:
    case BT_HCI_ERR_UNSUPP_LL_PARAM_VAL: return IoStatus::unsupported;
    case BT_HCI_ERR_INVALID_PARAM:
    case BT_HCI_ERR_INVALID_LL_PARAM:
    case BT_HCI_ERR_UNACCEPT_CONN_PARAM: return IoStatus::invalid_argument;
    case BT_HCI_ERR_UNKNOWN_CONN_ID: return IoStatus::device_lost;
    default: return IoStatus::io_failure;
    }
}

[[nodiscard]] IoStatus native_start_status(int error) noexcept {
    return error > 0 ? hci_status(static_cast<std::uint8_t>(error)) : errno_status(error);
}

[[nodiscard]] BleAddress portable_address(const bt_addr_le_t& address) noexcept {
    BleAddress result;
    std::copy(std::begin(address.a.val), std::end(address.a.val), result.bytes.begin());
    result.random = address.type == BT_ADDR_LE_RANDOM || address.type == BT_ADDR_LE_RANDOM_ID ||
                    address.type == BT_ADDR_LE_UNRESOLVED;
    return result;
}

[[nodiscard]] bt_addr_le_t native_address(const BleAddress& address) noexcept {
    bt_addr_le_t result{};
    result.type = address.random ? BT_ADDR_LE_RANDOM : BT_ADDR_LE_PUBLIC;
    std::copy(address.bytes.begin(), address.bytes.end(), std::begin(result.a.val));
    return result;
}

[[nodiscard]] std::uint16_t little_u16(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
}


[[nodiscard]] std::optional<BleUuid> portable_uuid(const bt_uuid* uuid) noexcept {
    if (uuid == nullptr) return std::nullopt;
    BleUuid result;
    switch (uuid->type) {
    case BT_UUID_TYPE_16: {
        const bt_uuid_16* native = BT_UUID_16(uuid);
        result.length = 2;
        result.bytes[0] = static_cast<std::uint8_t>(native->val);
        result.bytes[1] = static_cast<std::uint8_t>(native->val >> 8U);
        return result;
    }
    case BT_UUID_TYPE_128: {
        const bt_uuid_128* native = BT_UUID_128(uuid);
        result.length = 16;
        std::copy(std::begin(native->val), std::end(native->val), result.bytes.begin());
        return result;
    }
    default:
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<BleUuid> portable_uuid(const std::uint8_t* bytes,
                                                   std::size_t length) noexcept {
    bt_uuid_any native{};
    if (length == 2) {
        native.u16.uuid.type = BT_UUID_TYPE_16;
        native.u16.val = little_u16(bytes);
    } else if (length == 16) {
        native.u128.uuid.type = BT_UUID_TYPE_128;
        std::copy(bytes, bytes + length, std::begin(native.u128.val));
    } else {
        return std::nullopt;
    }
    return portable_uuid(&native.uuid);
}


enum class NativeWorkState : std::uint8_t { idle, queued, running, finishing, result_ready };
enum class NativePhase : std::uint8_t {
    none,
    scan_start,
    scan_wait,
    connect_start,
    connect_wait,
    attributes_start,
    services_start,
    services_wait,
    characteristics_start,
    characteristics_wait,
    attributes_finish,
    action_start,
    cccd_start,
    cccd_wait,
    read_start,
    read_wait,
    write_start,
    write_wait,
    subscribe_start,
    subscribe_wait,
    unsubscribe_start,
    unsubscribe_wait,
    disconnect_start,
    disconnect_wait,
    close_start,
    close_wait,
};

struct NativeTicket {
    DeviceHandle device{};
    DeviceHandle operation{};
    DeviceHandle subscription{};
    std::uint64_t owner_generation = 0;
    DeviceOperation id = DeviceOperation::none;
    BleDiscoveryKind discovery_kind = BleDiscoveryKind::scan;
    BleAttribute filter{};
    BleAddress peer{};
    BleWriteMode write_mode = BleWriteMode::with_response;
    BleSubscriptionTarget subscription_target = BleSubscriptionTarget::characteristic;
    std::uint32_t maximum_results = 0;
    std::uint32_t maximum_bytes = 0;
    std::uint32_t supervision_timeout_ms = 0;
    std::uint16_t value_length = 0;
};

struct NativeService {
    BleUuid uuid{};
    std::uint16_t start_handle = 0;
    std::uint16_t end_handle = 0;
};

struct NativeCharacteristic {
    BleAttribute attribute{};
    std::uint16_t declaration_handle = 0;
    std::uint16_t value_handle = 0;
    std::uint16_t range_end = 0;
    std::uint16_t cccd_handle = 0;
    std::uint16_t cccd_value = 0;
    std::uint8_t properties = 0;
};

struct BleNativeState;

struct NativeOperation {
    BleNativeState* owner = nullptr;
    NativeTicket ticket{};
    std::uint32_t sequence = 0;
    NativeWorkState state = NativeWorkState::idle;
    NativePhase phase = NativePhase::none;
    IoStatus terminal_status = IoStatus::completed;
    bool cancellation_requested = false;
    bool retirement_requested = false;
    bool cancel_cleanup_sent = false;
    std::uint32_t connection_generation = 0;
    std::uint16_t result_value_length = 0;
    std::uint16_t discovery_result_count = 0;
    std::uint16_t service_index = 0;
    std::uint16_t previous_service_handle = 0;
    std::uint16_t previous_characteristic_handle = 0;
    std::uint16_t pending_cccd_handle = 0;
    std::uint16_t pending_cccd_value = 0;
    std::uint16_t last_attribute_handle = 0;
    bool service_range_open = false;
    bool characteristic_range_open = false;
    std::uint8_t subscription_index = kNoIndex;
    BleAddress response_peer{};
    bool response_connected = false;
    bool response_paired = false;
    std::uint32_t response_connection_generation = 0;
    bt_le_conn_param connection_parameters{};
    bt_conn_le_create_param create_parameters{};
    bt_gatt_read_params read_parameters{};
    bt_gatt_write_params write_parameters{};
    std::array<std::uint8_t, zephyr_ble_central_max_attribute_bytes> value{};
    std::array<BleCharacteristicDescriptor, zephyr_ble_central_max_characteristics> results{};
};


struct NativeSubscription {
    BleNativeState* owner = nullptr;
    DeviceHandle subscription{};
    DeviceHandle device{};
    std::uint64_t owner_generation = 0;
    BleSubscriptionTarget target = BleSubscriptionTarget::characteristic;
    BleAttribute attribute{};
    std::uint32_t connection_generation = 0;
    std::uint16_t value_handle = 0;
    bool reserved = false;
    bool sdk_linked = false;
    bool accept_events = false;
    bt_gatt_subscribe_params parameters{};
};

struct NativeConnection {
    bt_conn* conn = nullptr;
    BleAddress peer{};

    std::uint32_t generation = 0;
    bool connected = false;
    bool paired = false;
    bool disconnect_event_pending = false;
    bt_conn* early_conn = nullptr;
    std::uint8_t early_connect_error = 0;
    bool create_in_progress = false;
    bool early_connect_event_pending = false;
    std::uint8_t early_disconnect_reason = 0;
    bool early_disconnect_pending = false;
    bool failed_connect_event_pending = false;
    bool disconnect_started = false;
    // Synchronous failures wait for a fresh teardown attempt; process must
    // not reschedule the same failed SDK call indefinitely.
    bool disconnect_failed = false;
    IoStatus disconnect_failure = IoStatus::completed;
    bool disconnect_cleanup = false;
    std::array<NativeService, zephyr_ble_central_max_services> services{};
    std::array<NativeCharacteristic, zephyr_ble_central_max_characteristics> characteristics{};
    std::uint16_t service_count = 0;
    std::uint16_t characteristic_count = 0;
    bool attributes_complete = false;
};

struct NativeNotification {
    DeviceHandle subscription{};
    DeviceHandle device{};
    std::uint64_t owner_generation = 0;
    std::uint32_t connection_generation = 0;
    BleSubscriptionTarget target = BleSubscriptionTarget::characteristic;
    BleAttribute attribute{};
    BleAddress peer{};
    bool connected = false;
    bool paired = false;
    std::uint16_t value_length = 0;
    std::array<std::uint8_t, zephyr_ble_central_max_attribute_bytes> value{};
};

struct ScanPeerRecord {
    BleAddress address{};
    std::int16_t rssi = 0;
    std::uint16_t advertisement_length = 0;
    std::array<std::uint8_t, zephyr_ble_central_max_advertisement_bytes> advertisement{};
};

struct ScanStore {
    k_spinlock lock{};
    BleNativeState* owner = nullptr;
    NativeOperation* operation = nullptr;
    bool scanning = false;
    bool stop_requested = false;
    IoStatus failure = IoStatus::completed;
    std::uint32_t maximum_results = 0;
    std::uint32_t maximum_bytes = 0;
    std::uint16_t peer_count = 0;
    std::array<ScanPeerRecord, zephyr_ble_central_max_scan_results> peers{};
};

ScanStore g_scan;

struct NativeStateRegistry {
    k_spinlock lock{};
    std::array<BleNativeState*, 8> states{};
    std::size_t count = 0;
    bool sdk_registered = false;
    bt_conn_cb callbacks{};
};

NativeStateRegistry g_native_states;

K_MUTEX_DEFINE(g_native_setup_mutex);
K_THREAD_STACK_DEFINE(g_native_ble_work_stack, 8192);
k_work_q g_native_ble_work_queue{};
bool g_native_ble_work_started = false;

[[nodiscard]] IoStatus start_native_ble_work_queue() noexcept {
    k_mutex_lock(&g_native_setup_mutex, K_FOREVER);
    if (!g_native_ble_work_started) {
        k_work_queue_start(&g_native_ble_work_queue, g_native_ble_work_stack,
                           K_THREAD_STACK_SIZEOF(g_native_ble_work_stack),
                           K_LOWEST_APPLICATION_THREAD_PRIO, nullptr);
        g_native_ble_work_started = true;
    }
    k_mutex_unlock(&g_native_setup_mutex);
    return k_work_queue_thread_get(&g_native_ble_work_queue) == nullptr
               ? IoStatus::io_failure
               : IoStatus::completed;
}

class NativeCallbackGuard {
public:
    NativeCallbackGuard() : key(k_spin_lock(&g_native_states.lock)) {}
    ~NativeCallbackGuard() { k_spin_unlock(&g_native_states.lock, key); }

    NativeCallbackGuard(const NativeCallbackGuard&) = delete;
    NativeCallbackGuard& operator=(const NativeCallbackGuard&) = delete;

private:
    k_spinlock_key_t key;
};

void connection_connected(bt_conn* conn, std::uint8_t error) noexcept;
void connection_disconnected(bt_conn* conn, std::uint8_t reason) noexcept;
#if defined(CONFIG_BT_SMP) || defined(CONFIG_BT_CLASSIC)
void connection_security_changed(bt_conn* conn, bt_security_t level,
                                  enum bt_security_err error) noexcept;
#endif

[[nodiscard]] IoStatus register_native_state(BleNativeState* state) noexcept;
void unregister_native_state(BleNativeState* state) noexcept;

static_assert(std::is_trivially_copyable_v<NativeTicket>);
static_assert(std::is_trivially_copyable_v<NativeOperation>);
static_assert(std::is_trivially_copyable_v<NativeSubscription>);
static_assert(std::is_trivially_copyable_v<NativeConnection>);
static_assert(std::is_trivially_copyable_v<NativeNotification>);
static_assert(std::is_trivially_copyable_v<ScanPeerRecord>);

struct BleNativeState {
    k_spinlock lock{};
    k_work work_item{};
    k_work_delayable scan_timer{};
    std::uint32_t scan_timeout_ms = 0;
    bool shutting_down = false;
    bool shutdown_complete = false;
    bool release_cleanup = false;
    bool close_cleanup = false;
    std::uint64_t lease_owner = 0;
    std::array<NativeOperation, zephyr_ble_central_max_pending_operations> operations{};
    std::array<NativeSubscription, zephyr_ble_central_max_subscriptions> subscriptions{};
    NativeConnection connection{};
    std::array<std::uint8_t, zephyr_ble_central_max_pending_operations> operation_queue{};
    std::size_t operation_head = 0;
    std::size_t operation_count = 0;
    std::uint8_t active_operation = kNoIndex;
    std::array<std::uint8_t, zephyr_ble_central_max_pending_operations> result_queue{};
    std::size_t result_head = 0;
    std::size_t result_count = 0;
    std::array<NativeNotification, zephyr_ble_central_max_notifications> notifications{};
    std::size_t notification_head = 0;
    std::size_t notification_count = 0;

    explicit BleNativeState(std::uint32_t timeout_ms);
    ~BleNativeState() = default;
    BleNativeState(const BleNativeState&) = delete;
    BleNativeState& operator=(const BleNativeState&) = delete;

    void process() noexcept;
    void wake() noexcept;

    static void run_work(k_work* work) noexcept;
    static void on_scan_timeout(k_work* work) noexcept;
    void handle_connected(bt_conn* conn, std::uint8_t error) noexcept;
    void handle_disconnected(bt_conn* conn, std::uint8_t reason) noexcept;
#if defined(CONFIG_BT_SMP) || defined(CONFIG_BT_CLASSIC)
    void handle_security_changed(bt_conn* conn, bt_security_t level,
                                 enum bt_security_err error) noexcept;
#endif
    static uint8_t on_attribute_read(bt_conn* conn, std::uint8_t error,
                                     bt_gatt_read_params* parameters,
                                     const void* data, std::uint16_t length) noexcept;
    static uint8_t on_read(bt_conn* conn, std::uint8_t error,
                           bt_gatt_read_params* parameters,
                           const void* data, std::uint16_t length) noexcept;
    static void on_written(bt_conn* conn, std::uint8_t error,
                           bt_gatt_write_params* parameters) noexcept;
    static void on_write_command_complete(bt_conn* conn, void* user_data) noexcept;
    static uint8_t on_notified(bt_conn* conn, bt_gatt_subscribe_params* parameters,
                               const void* data, std::uint16_t length) noexcept;
    static void on_subscribed(bt_conn* conn, std::uint8_t error,
                              bt_gatt_subscribe_params* parameters) noexcept;

    [[nodiscard]] NativeOperation& current_operation() noexcept {
        return operations[active_operation];
    }
    [[nodiscard]] bool current_is(const NativeOperation& operation) const noexcept {
        return active_operation != kNoIndex &&
               &operations[active_operation] == &operation;
    }
    [[nodiscard]] NativeSubscription& subscription_for(NativeOperation& operation) noexcept {
        return subscriptions[operation.subscription_index];
    }

    void finish(NativeOperation& operation, IoStatus status) noexcept;
    void finish_cancelled(NativeOperation& operation) noexcept;
    void queue_completed_results() noexcept;
    void apply_connection_events() noexcept;
    void publish_connection(bool connected) noexcept;
    void clear_attribute_cache() noexcept;
    void request_disconnect(bool cleanup) noexcept;
    void prepare_disconnect_retry() noexcept;
    [[nodiscard]] bool connected(const NativeOperation& operation) const noexcept;
    [[nodiscard]] NativeCharacteristic* find_characteristic(const BleAttribute& attribute) noexcept;
    void start_queued_operation() noexcept;
    [[nodiscard]] bool continue_current_operation() noexcept;

    void start_scan(NativeOperation& operation) noexcept;
    [[nodiscard]] bool continue_scan(NativeOperation& operation) noexcept;
    void start_connect(NativeOperation& operation) noexcept;
    void start_attributes(NativeOperation& operation) noexcept;
    void start_primary_discovery(NativeOperation& operation) noexcept;
    void start_characteristic_discovery(NativeOperation& operation) noexcept;
    void finish_attributes(NativeOperation& operation) noexcept;
    void start_action(NativeOperation& operation) noexcept;
    void start_cccd_discovery(NativeOperation& operation,
                              NativeCharacteristic& characteristic) noexcept;
    void start_uuid_read(NativeOperation& operation, const bt_uuid& uuid,
                         std::uint16_t start_handle, std::uint16_t end_handle,
                         NativePhase phase) noexcept;
    void fail_attribute_read(NativeOperation& operation, IoStatus status) noexcept;
    void complete_primary_discovery(NativeOperation& operation) noexcept;
    void complete_characteristic_discovery(NativeOperation& operation) noexcept;
    void complete_cccd_discovery(NativeOperation& operation) noexcept;
    void complete_attribute_read(NativeOperation& operation) noexcept;
    uint8_t process_primary_value(NativeOperation& operation, std::uint16_t handle,
                                  const std::uint8_t* data, std::uint16_t length) noexcept;
    uint8_t process_characteristic_value(NativeOperation& operation, std::uint16_t handle,
                                         const std::uint8_t* data,
                                         std::uint16_t length) noexcept;
    uint8_t process_cccd_value(NativeOperation& operation, std::uint16_t handle,
                               const std::uint8_t* data, std::uint16_t length) noexcept;
    void start_read(NativeOperation& operation,
                    const NativeCharacteristic& characteristic) noexcept;
    void start_write(NativeOperation& operation,
                     const NativeCharacteristic& characteristic) noexcept;
    void start_subscription(NativeOperation& operation,
                            NativeCharacteristic& characteristic) noexcept;
    void start_unsubscription(NativeOperation& operation) noexcept;
    void start_disconnection(NativeOperation& operation) noexcept;
    [[nodiscard]] bool continue_close(NativeOperation& operation) noexcept;
    void cancel_current_native_procedure(NativeOperation& operation) noexcept;
    [[nodiscard]] bool has_linked_subscriptions() const noexcept;
    [[nodiscard]] bool idle_for_destruction() const noexcept;
};

BleNativeState* native_from_work(k_work* work) noexcept {
    return CONTAINER_OF(work, BleNativeState, work_item);
}

BleNativeState* native_from_delayable(k_work_delayable* work) noexcept {
    return CONTAINER_OF(work, BleNativeState, scan_timer);
}

void scan_callback(const bt_addr_le_t* address, int8_t rssi,
                   std::uint8_t /*adv_type*/, net_buf_simple* buffer) noexcept {
    if (address == nullptr || buffer == nullptr) return;

    const auto key = k_spin_lock(&g_scan.lock);
    if (g_scan.owner != nullptr && g_scan.scanning && !g_scan.stop_requested) {
        const BleAddress packet_address = portable_address(*address);
        const std::size_t length = buffer->len;
        if (length > g_scan.peers[0].advertisement.size() || length > g_scan.maximum_bytes) {
            g_scan.failure = IoStatus::memory_exhausted;
            g_scan.stop_requested = true;
        } else {
            const auto existing = std::find_if(
                g_scan.peers.begin(), g_scan.peers.begin() + g_scan.peer_count,
                [&](const ScanPeerRecord& peer) { return same_address(peer.address, packet_address); });
            if (existing == g_scan.peers.begin() + g_scan.peer_count) {
                if (g_scan.peer_count == g_scan.peers.size()) {
                    g_scan.failure = IoStatus::memory_exhausted;
                    g_scan.stop_requested = true;
                } else {
                    ScanPeerRecord& record = g_scan.peers[g_scan.peer_count];
                    record.address = packet_address;
                    record.rssi = rssi;
                    record.advertisement_length = static_cast<std::uint16_t>(length);
                    if (length != 0) std::memcpy(record.advertisement.data(), buffer->data, length);
                    ++g_scan.peer_count;
                    if (g_scan.peer_count >= g_scan.maximum_results) {
                        g_scan.stop_requested = true;
                    }
                }
            }
        }
        if (g_scan.stop_requested && g_scan.owner != nullptr) {
            g_scan.owner->wake();
        }
    }
    k_spin_unlock(&g_scan.lock, key);
}

void scan_timer_expired(k_work* work) noexcept {
    BleNativeState* state = native_from_delayable(k_work_delayable_from_work(work));
    const auto key = k_spin_lock(&g_scan.lock);
    if (g_scan.owner == state && g_scan.scanning) {
        g_scan.stop_requested = true;
        k_spin_unlock(&g_scan.lock, key);
        state->wake();
        return;
    }
    k_spin_unlock(&g_scan.lock, key);
}

void release_scan_owner(BleNativeState* state, const NativeOperation* operation) noexcept {
    const auto key = k_spin_lock(&g_scan.lock);
    if (g_scan.owner == state && (operation == nullptr || g_scan.operation == operation)) {
        if (!g_scan.scanning) {
            g_scan.owner = nullptr;
            g_scan.operation = nullptr;
            g_scan.peer_count = 0;
            g_scan.maximum_results = 0;
            g_scan.maximum_bytes = 0;
            g_scan.failure = IoStatus::completed;
            g_scan.stop_requested = false;
        }
    }
    k_spin_unlock(&g_scan.lock, key);
}

BleNativeState::BleNativeState(std::uint32_t timeout_ms) : scan_timeout_ms(timeout_ms) {
    k_work_init(&work_item, &BleNativeState::run_work);
    k_work_init_delayable(&scan_timer, &BleNativeState::on_scan_timeout);
    for (NativeOperation& operation : operations) operation.owner = this;
    for (NativeSubscription& subscription : subscriptions) subscription.owner = this;
}

void BleNativeState::run_work(k_work* work) noexcept {
    native_from_work(work)->process();
}

void BleNativeState::on_scan_timeout(k_work* work) noexcept {
    scan_timer_expired(work);
}

void BleNativeState::wake() noexcept {
    (void)k_work_submit_to_queue(&g_native_ble_work_queue, &work_item);
}

void BleNativeState::finish(NativeOperation& operation, IoStatus status) noexcept {
    const auto key = k_spin_lock(&lock);
    if (operation.state != NativeWorkState::idle &&
        operation.state != NativeWorkState::finishing &&
        operation.state != NativeWorkState::result_ready) {
        operation.terminal_status = status;
        operation.phase = NativePhase::none;
        operation.state = NativeWorkState::finishing;
    }
    k_spin_unlock(&lock, key);
    wake();
}

void BleNativeState::finish_cancelled(NativeOperation& operation) noexcept {
    finish(operation, IoStatus::cancelled);
}

void BleNativeState::publish_connection(bool connected) noexcept {
    const auto key = k_spin_lock(&lock);
    for (NativeSubscription& subscription : subscriptions) {
        if (!subscription.reserved || !subscription.accept_events ||
            subscription.target != BleSubscriptionTarget::connection_state ||
            notification_count == notifications.size()) {
            continue;
        }
        const std::size_t tail = (notification_head + notification_count) % notifications.size();
        NativeNotification& event = notifications[tail];
        event.subscription = subscription.subscription;
        event.device = subscription.device;
        event.owner_generation = subscription.owner_generation;
        event.connection_generation = connection.generation;
        event.target = subscription.target;
        event.peer = connection.peer;
        event.connected = connected;
        event.paired = connected && connection.paired;
        event.value_length = 0;
        ++notification_count;
    }
    k_spin_unlock(&lock, key);
}




IoStatus register_native_state(BleNativeState* state) noexcept {
    const auto key = k_spin_lock(&g_native_states.lock);
    if (g_native_states.count == g_native_states.states.size()) {
        k_spin_unlock(&g_native_states.lock, key);
        return IoStatus::memory_exhausted;
    }
    if (!g_native_states.sdk_registered) {
        g_native_states.callbacks = {};
        g_native_states.callbacks.connected = &connection_connected;
        g_native_states.callbacks.disconnected = &connection_disconnected;
#if defined(CONFIG_BT_SMP) || defined(CONFIG_BT_CLASSIC)
        g_native_states.callbacks.security_changed = &connection_security_changed;
#endif
        const int error = bt_conn_cb_register(&g_native_states.callbacks);
        if (error != 0) {
            k_spin_unlock(&g_native_states.lock, key);
            return native_start_status(error);
        }
        g_native_states.sdk_registered = true;
    }
    g_native_states.states[g_native_states.count++] = state;
    k_spin_unlock(&g_native_states.lock, key);
    return IoStatus::completed;
}

void unregister_native_state(BleNativeState* state) noexcept {
    const auto key = k_spin_lock(&g_native_states.lock);
    for (std::size_t index = 0; index < g_native_states.count; ++index) {
        if (g_native_states.states[index] != state) continue;
        for (std::size_t move = index + 1; move < g_native_states.count; ++move) {
            g_native_states.states[move - 1] = g_native_states.states[move];
        }
        --g_native_states.count;
        g_native_states.states[g_native_states.count] = nullptr;
        break;
    }
    k_spin_unlock(&g_native_states.lock, key);
}

template <typename Callback>
void for_each_native_state(Callback callback) noexcept {
    // Removal takes the same lock, so a destructor cannot free a State while a
    // global SDK callback is inspecting its connection ownership.
    const auto key = k_spin_lock(&g_native_states.lock);
    for (std::size_t index = 0; index < g_native_states.count; ++index) {
        callback(*g_native_states.states[index]);
    }
    k_spin_unlock(&g_native_states.lock, key);
}

void connection_connected(bt_conn* conn, std::uint8_t error) noexcept {
    for_each_native_state([&](BleNativeState& state) { state.handle_connected(conn, error); });
}

void connection_disconnected(bt_conn* conn, std::uint8_t reason) noexcept {
    for_each_native_state([&](BleNativeState& state) { state.handle_disconnected(conn, reason); });
}

#if defined(CONFIG_BT_SMP) || defined(CONFIG_BT_CLASSIC)
void connection_security_changed(bt_conn* conn, bt_security_t level,
                                 enum bt_security_err error) noexcept {
    for_each_native_state(
        [&](BleNativeState& state) { state.handle_security_changed(conn, level, error); });
}
#endif

void BleNativeState::handle_connected(bt_conn* conn, std::uint8_t error) noexcept {
    bool accepted = false;
    bool role_failure = false;
    {
        const auto key = k_spin_lock(&lock);
        if (connection.conn != conn) {
            if (connection.create_in_progress && !connection.early_connect_event_pending &&
                active_operation != kNoIndex &&
                current_operation().ticket.id == DeviceOperation::connect &&
                current_operation().state == NativeWorkState::running) {
                bt_conn_info info{};
                const int info_error = bt_conn_get_info(conn, &info);
                const bt_addr_le_t* early_peer = info.le.dst != nullptr
                                                      ? info.le.dst
                                                      : info.le.remote;
                if (info_error == 0 && info.type == BT_CONN_TYPE_LE &&
                    info.role == BT_CONN_ROLE_CENTRAL && early_peer != nullptr &&
                    same_address(portable_address(*early_peer), connection.peer)) {
                    connection.early_conn = conn;
                    connection.early_connect_error = error;
                    connection.early_connect_event_pending = true;
                }
            }
            k_spin_unlock(&lock, key);
            return;
        }
        bool pending_connect = false;
        if (active_operation != kNoIndex) {
            NativeOperation& candidate = current_operation();
            pending_connect = candidate.ticket.id == DeviceOperation::connect &&
                              candidate.state == NativeWorkState::running;
        }
        if (!pending_connect) {
            if (error != 0) {
                connection.failed_connect_event_pending = true;
            } else {
                bt_conn_info info{};
                const int info_error = bt_conn_get_info(conn, &info);
                if (info_error == 0 && info.type == BT_CONN_TYPE_LE &&
                    info.le.dst != nullptr) {
                    connection.peer = portable_address(*info.le.dst);
                    connection.connected = info.state == BT_CONN_STATE_CONNECTED;
                    connection.paired = connection.connected &&
                                        info.security.level >= BT_SECURITY_L2;
                }
                connection.disconnect_cleanup = true;
                accepted = true;
            }
            k_spin_unlock(&lock, key);
            if (accepted && error == 0) publish_connection(true);
            wake();
            return;
        }
        NativeOperation& operation = current_operation();
        accepted = true;
        if (error != 0) {
            connection.failed_connect_event_pending = true;
            connection.connected = false;
            connection.paired = false;
            operation.terminal_status = operation.cancellation_requested
                                            ? IoStatus::cancelled
                                            : (error == BT_HCI_ERR_UNKNOWN_CONN_ID
                                                   ? IoStatus::timeout
                                                   : hci_status(error));
            operation.phase = NativePhase::none;
            operation.state = NativeWorkState::finishing;
            k_spin_unlock(&lock, key);
            wake();
            return;
        }

        bt_conn_info info{};
        const int info_error = bt_conn_get_info(conn, &info);
        if (info_error != 0 || info.type != BT_CONN_TYPE_LE || info.le.dst == nullptr) {
            operation.terminal_status = native_start_status(info_error != 0 ? info_error : -EPROTO);
            operation.state = NativeWorkState::finishing;
            operation.phase = NativePhase::none;
            connection.disconnect_cleanup = true;
            k_spin_unlock(&lock, key);
            wake();
            return;
        }
        connection.peer = portable_address(*info.le.dst);
        connection.connected = info.state == BT_CONN_STATE_CONNECTED;
        connection.paired = connection.connected && info.security.level >= BT_SECURITY_L2;
        role_failure = info.type != BT_CONN_TYPE_LE || info.role != BT_CONN_ROLE_CENTRAL;
        operation.response_peer = connection.peer;
        operation.response_connected = connection.connected && !role_failure;
        operation.response_paired = connection.paired && !role_failure;
        operation.response_connection_generation = connection.generation;
        operation.terminal_status = operation.cancellation_requested
                                        ? IoStatus::cancelled
                                        : (!connection.connected
                                               ? IoStatus::device_lost
                                               : (role_failure ? IoStatus::unsupported
                                                               : IoStatus::completed));
        operation.phase = NativePhase::none;
        operation.state = NativeWorkState::finishing;
        if (operation.cancellation_requested || role_failure || !connection.connected) {
            connection.disconnect_cleanup = true;
        }
        k_spin_unlock(&lock, key);
    }
    if (accepted && error == 0) publish_connection(true);
    wake();
}

void BleNativeState::handle_disconnected(bt_conn* conn, std::uint8_t reason) noexcept {
    (void)reason;
    {
        const auto key = k_spin_lock(&lock);
        if (connection.conn != conn) {
            if (connection.create_in_progress && connection.early_connect_event_pending &&
                connection.early_conn == conn) {
                connection.early_disconnect_pending = true;
                connection.early_disconnect_reason = reason;
            }
            k_spin_unlock(&lock, key);
            return;
        }
        connection.connected = false;
        connection.paired = false;
        connection.disconnect_event_pending = true;
        for (NativeOperation& operation : operations) {
            if ((operation.state != NativeWorkState::running &&
                 operation.state != NativeWorkState::finishing) ||
                operation.connection_generation != connection.generation) {
                continue;
            }
            if (operation.ticket.id == DeviceOperation::connect) {
                operation.response_connected = false;
                operation.response_paired = false;
                operation.terminal_status = operation.cancellation_requested
                                                ? IoStatus::cancelled
                                                : IoStatus::device_lost;
                operation.state = NativeWorkState::finishing;
                operation.phase = NativePhase::none;
                continue;
            }
            if (operation.state != NativeWorkState::running) continue;
            if (operation.ticket.id == DeviceOperation::disconnect) {
                operation.terminal_status = operation.cancellation_requested
                                                ? IoStatus::cancelled
                                                : IoStatus::completed;
                operation.state = NativeWorkState::finishing;
                operation.phase = NativePhase::none;
            } else if (operation.ticket.id != DeviceOperation::close) {
                operation.terminal_status = operation.cancellation_requested
                                                ? IoStatus::cancelled
                                                : IoStatus::device_lost;
                operation.state = NativeWorkState::finishing;
                operation.phase = NativePhase::none;
            }
        }
        k_spin_unlock(&lock, key);
    }
    publish_connection(false);
    wake();
}

#if defined(CONFIG_BT_SMP) || defined(CONFIG_BT_CLASSIC)
void BleNativeState::handle_security_changed(bt_conn* conn, bt_security_t level,
                                             enum bt_security_err /*error*/) noexcept {
    bool changed = false;
    {
        const auto key = k_spin_lock(&lock);
        if (connection.conn != conn || !connection.connected) {
            k_spin_unlock(&lock, key);
            return;
        }
        // Keep the existing metadata mapping: L2 indicates encryption, not
        // necessarily MITM authentication or a persisted bond.
        const bool paired = level >= BT_SECURITY_L2;
        changed = paired != connection.paired;
        connection.paired = paired;
        k_spin_unlock(&lock, key);
    }
    if (changed) publish_connection(true);
}
#endif

void BleNativeState::clear_attribute_cache() noexcept {
    connection.service_count = 0;
    connection.characteristic_count = 0;
    connection.attributes_complete = false;
    for (NativeService& service : connection.services) service = {};
    for (NativeCharacteristic& characteristic : connection.characteristics) {
        characteristic = {};
    }
}

bool BleNativeState::connected(const NativeOperation& operation) const noexcept {
    return connection.conn != nullptr && connection.connected &&
           connection.generation == operation.connection_generation;
}

NativeCharacteristic* BleNativeState::find_characteristic(const BleAttribute& attribute) noexcept {
    const auto found = std::find_if(
        connection.characteristics.begin(),
        connection.characteristics.begin() + connection.characteristic_count,
        [&](const NativeCharacteristic& characteristic) {
            return same_attribute(characteristic.attribute, attribute);
        });
    return found == connection.characteristics.begin() + connection.characteristic_count
               ? nullptr
               : &*found;
}


void BleNativeState::apply_connection_events() noexcept {
    bt_conn* failed = nullptr;
    bt_conn* disconnected = nullptr;
    std::uint32_t generation = 0;
    {
        const auto key = k_spin_lock(&lock);
        if (connection.failed_connect_event_pending) {
            connection.failed_connect_event_pending = false;
            failed = connection.conn;
            connection.conn = nullptr;
            connection.connected = false;
            connection.paired = false;
            connection.disconnect_started = false;
            clear_attribute_cache();
        } else if (connection.disconnect_event_pending) {
            connection.disconnect_event_pending = false;
            disconnected = connection.conn;
            connection.conn = nullptr;
            generation = connection.generation;
            connection.connected = false;
            connection.paired = false;
            connection.disconnect_started = false;
            clear_attribute_cache();
            for (NativeSubscription& subscription : subscriptions) {
                if (subscription.target == BleSubscriptionTarget::characteristic &&
                    subscription.connection_generation == generation) {
                    subscription.accept_events = false;
                }
            }
        }
        k_spin_unlock(&lock, key);
    }
    if (failed != nullptr) bt_conn_unref(failed);
    if (disconnected != nullptr) bt_conn_unref(disconnected);
}

void BleNativeState::prepare_disconnect_retry() noexcept {
    const auto key = k_spin_lock(&lock);
    connection.disconnect_failed = false;
    connection.disconnect_failure = IoStatus::completed;
    k_spin_unlock(&lock, key);
}

void BleNativeState::request_disconnect(bool cleanup) noexcept {
    bt_conn* conn = nullptr;
    {
        const auto key = k_spin_lock(&lock);
        connection.disconnect_cleanup = connection.disconnect_cleanup || cleanup;
        if (connection.conn == nullptr || connection.disconnect_started ||
            connection.disconnect_failed || connection.failed_connect_event_pending ||
            connection.disconnect_event_pending) {
            k_spin_unlock(&lock, key);
            return;
        }
        connection.disconnect_started = true;
        conn = connection.conn;
        k_spin_unlock(&lock, key);
    }
    const int error = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    if (error == 0) return;
    const auto key = k_spin_lock(&lock);

    connection.disconnect_started = false;
    connection.disconnect_failed = true;
    connection.disconnect_failure = native_start_status(error);
    if (active_operation != kNoIndex) {
        NativeOperation& operation = current_operation();
        if ((operation.ticket.id == DeviceOperation::disconnect ||
             operation.ticket.id == DeviceOperation::connect ||
             operation.ticket.id == DeviceOperation::close) &&
            operation.state == NativeWorkState::running) {
            operation.terminal_status = operation.cancellation_requested
                                            ? IoStatus::cancelled
                                            : native_start_status(error);
            operation.phase = NativePhase::none;
            operation.state = NativeWorkState::finishing;
        }
    }
    k_spin_unlock(&lock, key);
    wake();
}

bool BleNativeState::has_linked_subscriptions() const noexcept {
    return std::any_of(subscriptions.begin(), subscriptions.end(),
                       [](const NativeSubscription& subscription) {
                           return subscription.sdk_linked;
                       });
}

bool BleNativeState::idle_for_destruction() const noexcept {
    if (connection.conn != nullptr || operation_count != 0 || active_operation != kNoIndex ||
        result_count != 0 || notification_count != 0 || has_linked_subscriptions()) {
        return false;
    }
    return std::all_of(operations.begin(), operations.end(),
                       [](const NativeOperation& operation) {
                           return operation.state == NativeWorkState::idle;
                       });
}

void BleNativeState::queue_completed_results() noexcept {
    const auto key = k_spin_lock(&lock);
    for (std::size_t index = 0; index < operations.size(); ++index) {
        NativeOperation& operation = operations[index];
        if (operation.state != NativeWorkState::finishing) continue;
        if (operation.cancellation_requested) {
            operation.terminal_status = IoStatus::cancelled;
        }
        operation.state = NativeWorkState::result_ready;
        result_queue[(result_head + result_count) % result_queue.size()] =
            static_cast<std::uint8_t>(index);
        ++result_count;
        if (active_operation == index) active_operation = kNoIndex;
    }
    k_spin_unlock(&lock, key);
}

void BleNativeState::process() noexcept {
    for (;;) {
        queue_completed_results();
        apply_connection_events();

        bool disconnect_allowed = false;
        bt_conn* conn = nullptr;
        {
            const auto key = k_spin_lock(&lock);
            disconnect_allowed = !connection.disconnect_failed;
            conn = connection.conn;
            k_spin_unlock(&lock, key);
        }
        if (conn != nullptr && disconnect_allowed &&
            (shutting_down || release_cleanup || close_cleanup ||
             connection.disconnect_cleanup)) {
            request_disconnect(true);
        }

        if (active_operation != kNoIndex) {
            if (!continue_current_operation()) return;
            continue;
        }
        bool queued = false;
        {
            const auto key = k_spin_lock(&lock);
            queued = operation_count != 0;
            k_spin_unlock(&lock, key);
        }
        if (queued) {
            start_queued_operation();
            continue;
        }
        bool finish_cleanup = false;
        {
            const auto key = k_spin_lock(&lock);
            finish_cleanup = !shutting_down && conn == nullptr && !has_linked_subscriptions();
            if (finish_cleanup) {
                if (release_cleanup || close_cleanup) {
                    for (NativeSubscription& subscription : subscriptions) {
                        subscription.reserved = false;
                    }
                }
                release_cleanup = false;
                close_cleanup = false;
                connection.disconnect_cleanup = false;
            }
            k_spin_unlock(&lock, key);
        }
        (void)finish_cleanup;
        return;
    }
}

void BleNativeState::start_queued_operation() noexcept {
    const auto key = k_spin_lock(&lock);
    const std::size_t index = operation_queue[operation_head];
    operation_head = (operation_head + 1) % operation_queue.size();
    --operation_count;
    active_operation = static_cast<std::uint8_t>(index);
    NativeOperation& operation = operations[index];
    operation.state = NativeWorkState::running;
    if (operation.ticket.id != DeviceOperation::unsubscribe &&
        (operation.cancellation_requested || operation.retirement_requested ||
         (shutting_down && operation.ticket.id != DeviceOperation::close))) {
        k_spin_unlock(&lock, key);
        finish_cancelled(operation);
        return;
    }
    if (operation.ticket.id == DeviceOperation::subscribe &&
        operation.ticket.subscription_target == BleSubscriptionTarget::connection_state) {
        subscriptions[operation.subscription_index].accept_events = true;
    }
    k_spin_unlock(&lock, key);
    switch (operation.ticket.id) {
    case DeviceOperation::open:
    case DeviceOperation::subscribe:
        if (operation.ticket.id == DeviceOperation::open ||
            operation.ticket.subscription_target == BleSubscriptionTarget::connection_state) {
            finish(operation, IoStatus::completed);
            return;
        }
        operation.phase = NativePhase::attributes_start;
        return;
    case DeviceOperation::discover:
        operation.phase = operation.ticket.discovery_kind == BleDiscoveryKind::scan
                              ? NativePhase::scan_start
                              : NativePhase::attributes_start;
        return;
    case DeviceOperation::connect: operation.phase = NativePhase::connect_start; return;
    case DeviceOperation::disconnect: operation.phase = NativePhase::disconnect_start; return;
    case DeviceOperation::read:
    case DeviceOperation::write: operation.phase = NativePhase::attributes_start; return;
    case DeviceOperation::unsubscribe: operation.phase = NativePhase::unsubscribe_start; return;
    case DeviceOperation::close: operation.phase = NativePhase::close_start; return;
    default: finish(operation, IoStatus::unsupported); return;
    }
}

bool BleNativeState::continue_current_operation() noexcept {
    NativeOperation& operation = current_operation();
    bool cancelled = false;
    bool cancel_sent = false;
    NativePhase phase = NativePhase::none;
    {
        const auto key = k_spin_lock(&lock);
        if (operation.state != NativeWorkState::running) {
            k_spin_unlock(&lock, key);
            return false;
        }
        cancelled = operation.cancellation_requested;
        cancel_sent = operation.cancel_cleanup_sent;
        k_spin_unlock(&lock, key);
    }
    if (cancelled && !cancel_sent) cancel_current_native_procedure(operation);
    {
        const auto key = k_spin_lock(&lock);
        if (operation.state != NativeWorkState::running) {
            k_spin_unlock(&lock, key);
            return false;
        }
        phase = operation.phase;
        k_spin_unlock(&lock, key);
    }
    if (phase == NativePhase::disconnect_start || phase == NativePhase::close_start) {
        prepare_disconnect_retry();
    }
    switch (phase) {
    case NativePhase::scan_start: start_scan(operation); return true;
    case NativePhase::scan_wait: return continue_scan(operation);
    case NativePhase::connect_start: start_connect(operation); return true;
    case NativePhase::connect_wait: return false;
    case NativePhase::attributes_start: start_attributes(operation); return true;
    case NativePhase::services_start: start_primary_discovery(operation); return true;
    case NativePhase::services_wait:
    case NativePhase::characteristics_wait:
    case NativePhase::cccd_wait:
    case NativePhase::read_wait:
    case NativePhase::write_wait:
    case NativePhase::subscribe_wait:
    case NativePhase::unsubscribe_wait:
    case NativePhase::disconnect_wait: return false;
    case NativePhase::close_wait: return continue_close(operation);
    case NativePhase::close_start: return continue_close(operation);
    case NativePhase::characteristics_start: start_characteristic_discovery(operation); return true;
    case NativePhase::attributes_finish: finish_attributes(operation); return true;
    case NativePhase::action_start: start_action(operation); return true;
    case NativePhase::cccd_start: {
        NativeCharacteristic* characteristic = find_characteristic(operation.ticket.filter);
        if (characteristic == nullptr) {
            finish(operation, IoStatus::invalid_argument);
        } else {
            start_cccd_discovery(operation, *characteristic);
        }
        return true;
    }
    case NativePhase::read_start:
    case NativePhase::write_start:
    case NativePhase::subscribe_start: start_action(operation); return true;
    case NativePhase::unsubscribe_start: start_unsubscription(operation); return true;
    case NativePhase::disconnect_start: start_disconnection(operation); return true;
    case NativePhase::none:
        finish(operation, operation.terminal_status);
        return true;
    default:
        finish(operation, IoStatus::unsupported);
        return true;
    }
}

void BleNativeState::cancel_current_native_procedure(NativeOperation& operation) noexcept {
    operation.cancel_cleanup_sent = true;
    switch (operation.phase) {
    case NativePhase::connect_wait:
        connection.disconnect_cleanup = true;
        request_disconnect(true);
        return;
    case NativePhase::attributes_start:
        finish_cancelled(operation);
        return;
    case NativePhase::services_start:
    case NativePhase::characteristics_start:
        fail_attribute_read(operation, IoStatus::cancelled);
        return;
    case NativePhase::attributes_finish:
    case NativePhase::action_start:
    case NativePhase::cccd_start:
    case NativePhase::read_start:
    case NativePhase::write_start:
    case NativePhase::subscribe_start:
        finish_cancelled(operation);
        return;
    case NativePhase::unsubscribe_start: return;
    case NativePhase::services_wait:
    case NativePhase::characteristics_wait:
    case NativePhase::cccd_wait:
    case NativePhase::read_wait:
        if (connection.conn != nullptr) bt_gatt_cancel(connection.conn, &operation.read_parameters);
        return;
    case NativePhase::write_wait:
        if (operation.ticket.write_mode == BleWriteMode::with_response &&
            connection.conn != nullptr) {
            bt_gatt_cancel(connection.conn, &operation.write_parameters);
        }
        return;
    case NativePhase::subscribe_wait: return;
    case NativePhase::disconnect_wait:
    case NativePhase::close_start:
    case NativePhase::close_wait:
        return;
    default:
        return;
    }
}
void BleNativeState::start_scan(NativeOperation& operation) noexcept {
    {
        const auto scan_key = k_spin_lock(&g_scan.lock);
        if (g_scan.owner != nullptr) {
            k_spin_unlock(&g_scan.lock, scan_key);
            finish(operation, IoStatus::busy);
            return;
        }
        g_scan.owner = this;
        g_scan.operation = &operation;
        g_scan.maximum_results = operation.ticket.maximum_results;
        g_scan.maximum_bytes = operation.ticket.maximum_bytes;
        g_scan.peer_count = 0;
        g_scan.failure = IoStatus::completed;
        g_scan.stop_requested = false;
        g_scan.scanning = true;
        k_spin_unlock(&g_scan.lock, scan_key);
    }

    bt_le_scan_param parameters = BT_LE_SCAN_PASSIVE[0];
    parameters.timeout = 0;
    const auto key = k_spin_lock(&lock);
    operation.phase = NativePhase::scan_wait;
    k_spin_unlock(&lock, key);
    if (k_work_schedule(&scan_timer, K_MSEC(scan_timeout_ms)) < 0) {
        finish(operation, IoStatus::io_failure);
        return;
    }
    const int error = bt_le_scan_start(&parameters, &scan_callback);
    if (error != 0) {
        (void)k_work_cancel_delayable(&scan_timer);
        const auto scan_key = k_spin_lock(&g_scan.lock);
        g_scan.scanning = false;
        k_spin_unlock(&g_scan.lock, scan_key);
        release_scan_owner(this, &operation);
        finish(operation, native_start_status(error));
    }
}

bool BleNativeState::continue_scan(NativeOperation& operation) noexcept {
    bool stop = operation.cancellation_requested;
    IoStatus failure = IoStatus::completed;
    {
        const auto key = k_spin_lock(&g_scan.lock);
        if (g_scan.owner == this && g_scan.operation == &operation) {
            stop = stop || g_scan.stop_requested;
            failure = g_scan.failure;
        } else {
            finish(operation, IoStatus::device_lost);
            return true;
        }
        k_spin_unlock(&g_scan.lock, key);
    }
    if (!stop) return false;

    const int error = bt_le_scan_stop();
    (void)k_work_cancel_delayable(&scan_timer);
    const auto key = k_spin_lock(&g_scan.lock);
    if (g_scan.owner == this && g_scan.operation == &operation) {
        g_scan.scanning = false;
        g_scan.failure = failure != IoStatus::completed
                             ? failure
                             : (operation.cancellation_requested
                                    ? IoStatus::cancelled
                                    : native_start_status(error));
    }
    k_spin_unlock(&g_scan.lock, key);
    finish(operation, operation.cancellation_requested
                          ? IoStatus::cancelled
                          : (failure != IoStatus::completed ? failure
                                                             : native_start_status(error)));
    return true;
}

void BleNativeState::start_connect(NativeOperation& operation) noexcept {
    {
        const auto scan_key = k_spin_lock(&g_scan.lock);
        const bool scanning = g_scan.scanning;
        k_spin_unlock(&g_scan.lock, scan_key);
        if (scanning) {
            finish(operation, IoStatus::busy);
            return;
        }
        if (connection.conn != nullptr) {
            finish(operation, connection.connected ? IoStatus::busy : IoStatus::device_lost);
            return;
        }
    }

    clear_attribute_cache();
    ++connection.generation;
    if (connection.generation == 0) ++connection.generation;
    connection.peer = operation.ticket.peer;
    operation.connection_generation = connection.generation;
    operation.connection_parameters = BT_LE_CONN_PARAM_DEFAULT[0];
    operation.connection_parameters.timeout = static_cast<std::uint16_t>(
        operation.ticket.supervision_timeout_ms / 10U);
    operation.create_parameters = BT_CONN_LE_CREATE_CONN[0];
    operation.create_parameters.timeout = static_cast<std::uint16_t>(
        zephyr_ble_central_connect_attempt_timeout_ms / 10U);
    const bt_addr_le_t peer = native_address(operation.ticket.peer);
    {
        const auto key = k_spin_lock(&lock);
        connection.conn = nullptr;
        connection.connected = false;
        connection.paired = false;
        connection.disconnect_started = false;
        connection.disconnect_failed = false;
        connection.disconnect_failure = IoStatus::completed;
        connection.failed_connect_event_pending = false;
        connection.disconnect_event_pending = false;
        connection.create_in_progress = true;
        connection.early_connect_event_pending = false;
        connection.early_disconnect_pending = false;
        connection.early_disconnect_reason = 0;
        operation.phase = NativePhase::connect_wait;
        k_spin_unlock(&lock, key);
    }
    bt_conn* created = nullptr;
    const int error = bt_conn_le_create(&peer, &operation.create_parameters,
                                        &operation.connection_parameters, &created);
    bt_conn* early_conn = nullptr;
    std::uint8_t early_error = 0;
    bool early_event = false;
    bool early_disconnect = false;
    std::uint8_t early_disconnect_reason = 0;
    {
        const auto key = k_spin_lock(&lock);
        connection.conn = error == 0 ? created : nullptr;
        connection.create_in_progress = false;
        early_conn = connection.early_conn;
        early_error = connection.early_connect_error;
        early_event = connection.early_connect_event_pending;
        early_disconnect = connection.early_disconnect_pending;
        early_disconnect_reason = connection.early_disconnect_reason;
        connection.early_disconnect_pending = false;
        connection.early_disconnect_reason = 0;
        connection.early_conn = nullptr;
        connection.early_connect_error = 0;
        connection.early_connect_event_pending = false;
        k_spin_unlock(&lock, key);
    }
    if (early_event) {
        NativeCallbackGuard callback_guard;
        handle_connected(early_conn, early_error);
        if (early_disconnect) handle_disconnected(early_conn, early_disconnect_reason);
    }
    if (error != 0) {
        finish(operation, native_start_status(error));
    } else if (created == nullptr) {
        finish(operation, IoStatus::io_failure);
    }
}

void BleNativeState::start_attributes(NativeOperation& operation) noexcept {
    if (!connected(operation)) {
        finish(operation, IoStatus::device_lost);
        return;
    }
    if (connection.attributes_complete && operation.ticket.id != DeviceOperation::discover) {
        operation.phase = NativePhase::action_start;
        return;
    }
    clear_attribute_cache();
    operation.discovery_result_count = 0;
    operation.service_index = 0;
    operation.previous_service_handle = 0;
    operation.previous_characteristic_handle = 0;
    operation.pending_cccd_handle = 0;
    operation.pending_cccd_value = 0;
    operation.service_range_open = false;
    operation.characteristic_range_open = false;
    operation.phase = NativePhase::services_start;
}

void BleNativeState::start_uuid_read(NativeOperation& operation, const bt_uuid& uuid,
                                     std::uint16_t start_handle, std::uint16_t end_handle,
                                     NativePhase phase) noexcept {
    operation.last_attribute_handle = 0;
    operation.read_parameters = {};
    operation.read_parameters.func = &BleNativeState::on_attribute_read;
    operation.read_parameters.handle_count = 0;
    operation.read_parameters.by_uuid.uuid = &uuid;
    operation.read_parameters.by_uuid.start_handle = start_handle;
    operation.read_parameters.by_uuid.end_handle = end_handle;
#if defined(CONFIG_BT_EATT)
    operation.read_parameters.chan_opt = BT_ATT_CHAN_OPT_NONE;
#endif
    operation.phase = phase;
    const int error = bt_gatt_read(connection.conn, &operation.read_parameters);
    if (error != 0) fail_attribute_read(operation, native_start_status(error));
}

void BleNativeState::start_primary_discovery(NativeOperation& operation) noexcept {
    if (!connected(operation)) {
        finish(operation, IoStatus::device_lost);
        return;
    }
    operation.previous_service_handle = 0;
    operation.service_range_open = false;
    start_uuid_read(operation, kPrimaryServiceUuid.uuid, BT_ATT_FIRST_ATTRIBUTE_HANDLE,
                    BT_ATT_LAST_ATTRIBUTE_HANDLE, NativePhase::services_wait);
}

void BleNativeState::start_characteristic_discovery(NativeOperation& operation) noexcept {
    if (!connected(operation)) {
        fail_attribute_read(operation, IoStatus::device_lost);
        return;
    }
    if (operation.service_index >= connection.service_count) {
        operation.phase = NativePhase::attributes_finish;
        return;
    }
    const NativeService& service = connection.services[operation.service_index];
    if (service.start_handle == 0 || service.end_handle < service.start_handle) {
        fail_attribute_read(operation, att_status(BT_ATT_ERR_INVALID_PDU));
        return;
    }
    if (service.start_handle == service.end_handle) {
        ++operation.service_index;
        return;
    }
    operation.previous_characteristic_handle = service.start_handle;
    operation.characteristic_range_open = false;
    start_uuid_read(operation, kCharacteristicUuid.uuid,
                    static_cast<std::uint16_t>(service.start_handle + 1U), service.end_handle,
                    NativePhase::characteristics_wait);
}

void BleNativeState::finish_attributes(NativeOperation& operation) noexcept {
    if (!connected(operation)) {
        finish(operation, IoStatus::device_lost);
        return;
    }
    if (operation.ticket.id == DeviceOperation::discover) {
        connection.attributes_complete = wildcard_attribute(operation.ticket.filter);
        finish(operation, IoStatus::completed);
        return;
    }
    connection.attributes_complete = true;
    operation.phase = NativePhase::action_start;
}

void BleNativeState::start_action(NativeOperation& operation) noexcept {
    if (!connected(operation)) {
        finish(operation, IoStatus::device_lost);
        return;
    }
    switch (operation.ticket.id) {
    case DeviceOperation::read: {
        NativeCharacteristic* characteristic = find_characteristic(operation.ticket.filter);
        if (characteristic == nullptr) {
            finish(operation, IoStatus::invalid_argument);
        } else if ((characteristic->properties & BT_GATT_CHRC_READ) == 0) {
            finish(operation, IoStatus::unsupported);
        } else {
            start_read(operation, *characteristic);
        }
        return;
    }
    case DeviceOperation::write: {
        NativeCharacteristic* characteristic = find_characteristic(operation.ticket.filter);
        if (characteristic == nullptr) {
            finish(operation, IoStatus::invalid_argument);
            return;
        }
        const std::uint8_t property = operation.ticket.write_mode == BleWriteMode::with_response
                                          ? BT_GATT_CHRC_WRITE
                                          : BT_GATT_CHRC_WRITE_WITHOUT_RESP;
        if ((characteristic->properties & property) == 0) {
            finish(operation, IoStatus::unsupported);
        } else {
            start_write(operation, *characteristic);
        }
        return;
    }
    case DeviceOperation::subscribe: {
        NativeCharacteristic* characteristic = find_characteristic(operation.ticket.filter);
        if (characteristic == nullptr) {
            finish(operation, IoStatus::invalid_argument);
        } else if ((characteristic->properties &
                    (BT_GATT_CHRC_NOTIFY | BT_GATT_CHRC_INDICATE)) == 0) {
            finish(operation, IoStatus::unsupported);
        } else if (characteristic->cccd_handle == 0) {
            start_cccd_discovery(operation, *characteristic);
        } else {
            start_subscription(operation, *characteristic);
        }
        return;
    }
    default: finish(operation, IoStatus::unsupported); return;
    }
}

void BleNativeState::start_cccd_discovery(
    NativeOperation& operation, NativeCharacteristic& characteristic) noexcept {
    if (!connected(operation)) {
        finish(operation, IoStatus::device_lost);
        return;
    }
    operation.pending_cccd_handle = 0;
    operation.pending_cccd_value = 0;
    if (characteristic.value_handle >= characteristic.range_end) {
        fail_attribute_read(operation, IoStatus::unsupported);
        return;
    }
    start_uuid_read(operation, kCccdUuid.uuid,
                    static_cast<std::uint16_t>(characteristic.value_handle + 1U),
                    characteristic.range_end, NativePhase::cccd_wait);
}

void BleNativeState::start_read(
    NativeOperation& operation, const NativeCharacteristic& characteristic) noexcept {
    if (operation.ticket.maximum_bytes == 0) {
        finish(operation, IoStatus::memory_exhausted);
        return;
    }
    operation.result_value_length = 0;
    operation.read_parameters = {};
    operation.read_parameters.func = &BleNativeState::on_read;
    operation.read_parameters.handle_count = 1;
    operation.read_parameters.single.handle = characteristic.value_handle;
    operation.read_parameters.single.offset = 0;
#if defined(CONFIG_BT_EATT)
    operation.read_parameters.chan_opt = BT_ATT_CHAN_OPT_NONE;
#endif
    operation.phase = NativePhase::read_wait;
    const int error = bt_gatt_read(connection.conn, &operation.read_parameters);
    if (error != 0) finish(operation, native_start_status(error));
}

void BleNativeState::start_write(
    NativeOperation& operation, const NativeCharacteristic& characteristic) noexcept {
    const std::uint16_t mtu = bt_gatt_get_mtu(connection.conn);
    if (mtu < 3) {
        finish(operation, IoStatus::device_lost);
        return;
    }
    if (operation.ticket.value_length > static_cast<std::uint16_t>(mtu - 3U)) {
        finish(operation, IoStatus::invalid_argument);
        return;
    }
    operation.result_value_length = 0;
    operation.phase = NativePhase::write_wait;
    int error = 0;
    if (operation.ticket.write_mode == BleWriteMode::without_response) {
        error = bt_gatt_write_without_response_cb(
            connection.conn, characteristic.value_handle, operation.value.data(),
            operation.ticket.value_length, false, &BleNativeState::on_write_command_complete,
            &operation);
    } else {
        operation.write_parameters = {};
        operation.write_parameters.func = &BleNativeState::on_written;
        operation.write_parameters.handle = characteristic.value_handle;
        operation.write_parameters.offset = 0;
        operation.write_parameters.data = operation.value.data();
        operation.write_parameters.length = operation.ticket.value_length;
#if defined(CONFIG_BT_EATT)
        operation.write_parameters.chan_opt = BT_ATT_CHAN_OPT_NONE;
#endif
        error = bt_gatt_write(connection.conn, &operation.write_parameters);
    }
    if (error != 0) finish(operation, native_start_status(error));
}

void BleNativeState::start_subscription(
    NativeOperation& operation, NativeCharacteristic& characteristic) noexcept {
    if (operation.subscription_index == kNoIndex) {
        finish(operation, IoStatus::invalid_argument);
        return;
    }
    NativeSubscription& subscription = subscription_for(operation);
    subscription.sdk_linked = true;
    subscription.accept_events = false;
    subscription.value_handle = characteristic.value_handle;
    subscription.connection_generation = operation.connection_generation;
    subscription.parameters = {};
    subscription.parameters.notify = &BleNativeState::on_notified;
    subscription.parameters.subscribe = &BleNativeState::on_subscribed;
    subscription.parameters.value_handle = characteristic.value_handle;
    subscription.parameters.ccc_handle = characteristic.cccd_handle;
    subscription.parameters.value = (characteristic.properties & BT_GATT_CHRC_NOTIFY) != 0
                                        ? BT_GATT_CCC_NOTIFY
                                        : BT_GATT_CCC_INDICATE;
    atomic_set_bit(subscription.parameters.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);
#if defined(CONFIG_BT_SMP)
    subscription.parameters.min_security = BT_SECURITY_L1;
#endif
#if defined(CONFIG_BT_EATT)
    subscription.parameters.chan_opt = BT_ATT_CHAN_OPT_NONE;
#endif
    operation.phase = NativePhase::subscribe_wait;
    const int error = bt_gatt_subscribe(connection.conn, &subscription.parameters);
    if (error == 0) return;
    subscription.sdk_linked = false;
    subscription.accept_events = false;
    finish(operation, native_start_status(error));
}

void BleNativeState::start_unsubscription(NativeOperation& operation) noexcept {
    if (operation.subscription_index == kNoIndex) {
        finish(operation, IoStatus::invalid_argument);
        return;
    }
    NativeSubscription& subscription = subscription_for(operation);
    subscription.accept_events = false;
    if (!subscription.sdk_linked || subscription.target == BleSubscriptionTarget::connection_state) {
        subscription.sdk_linked = false;
        subscription.reserved = false;
        finish(operation, operation.cancellation_requested ? IoStatus::cancelled
                                                           : IoStatus::completed);
        return;
    }
    if (!connected(operation)) {
        finish(operation, IoStatus::device_lost);
        return;
    }
    operation.phase = NativePhase::unsubscribe_wait;
    const int error = bt_gatt_unsubscribe(connection.conn, &subscription.parameters);
    if (error != 0) {
        finish(operation, native_start_status(error));
        connection.disconnect_cleanup = true;
        request_disconnect(true);
    }
}

void BleNativeState::start_disconnection(NativeOperation& operation) noexcept {
    if (connection.disconnect_failed) {
        finish(operation, connection.disconnect_failure);
        return;
    }
    if (!connected(operation) || !same_address(connection.peer, operation.ticket.peer)) {
        finish(operation, IoStatus::invalid_argument);
        return;
    }
    operation.phase = NativePhase::disconnect_wait;
    request_disconnect(true);
}

bool BleNativeState::continue_close(NativeOperation& operation) noexcept {
    if (connection.conn != nullptr) {
        close_cleanup = true;
        if (connection.disconnect_failed) {
            finish(operation, connection.disconnect_failure);
            return true;
        }
        operation.phase = NativePhase::close_wait;
        if (!connection.disconnect_started) request_disconnect(true);
        return false;
    }
    bool linked = false;
    {
        const auto key = k_spin_lock(&lock);
        linked = has_linked_subscriptions();
        if (!linked) {
            for (NativeSubscription& subscription : subscriptions) {
                subscription.reserved = false;
            }
        }
        k_spin_unlock(&lock, key);
    }
    if (linked) {
        finish(operation, IoStatus::device_lost);
        return true;
    }
    close_cleanup = false;
    finish(operation, IoStatus::completed);
    return true;
}

void BleNativeState::fail_attribute_read(NativeOperation& operation,
                                           IoStatus status) noexcept {
    switch (operation.phase) {
    case NativePhase::services_start:
    case NativePhase::services_wait:
    case NativePhase::characteristics_start:
    case NativePhase::characteristics_wait:
        clear_attribute_cache();
        operation.discovery_result_count = 0;
        operation.service_range_open = false;
        operation.characteristic_range_open = false;
        break;
    case NativePhase::cccd_start:
    case NativePhase::cccd_wait:
        operation.pending_cccd_handle = 0;
        operation.pending_cccd_value = 0;
        break;
    default:
        break;
    }
    finish(operation, operation.cancellation_requested || operation.retirement_requested
                          ? IoStatus::cancelled
                          : status);
}

void BleNativeState::complete_primary_discovery(NativeOperation& operation) noexcept {
    if (operation.service_range_open) {
        if (connection.service_count == 0) {
            fail_attribute_read(operation, att_status(BT_ATT_ERR_INVALID_PDU));
            return;
        }
        connection.services[connection.service_count - 1].end_handle =
            BT_ATT_LAST_ATTRIBUTE_HANDLE;
        operation.service_range_open = false;
    }
    operation.phase = connection.service_count == 0 ? NativePhase::attributes_finish
                                                    : NativePhase::characteristics_start;
    wake();
}

void BleNativeState::complete_characteristic_discovery(NativeOperation& operation) noexcept {
    if (operation.service_index >= connection.service_count) {
        fail_attribute_read(operation, att_status(BT_ATT_ERR_INVALID_PDU));
        return;
    }
    if (operation.characteristic_range_open) {
        if (connection.characteristic_count == 0) {
            fail_attribute_read(operation, att_status(BT_ATT_ERR_INVALID_PDU));
            return;
        }
        connection.characteristics[connection.characteristic_count - 1].range_end =
            connection.services[operation.service_index].end_handle;
        operation.characteristic_range_open = false;
    }
    ++operation.service_index;
    operation.phase = NativePhase::characteristics_start;
    wake();
}

void BleNativeState::complete_cccd_discovery(NativeOperation& operation) noexcept {
    if (operation.pending_cccd_handle == 0) {
        fail_attribute_read(operation, IoStatus::unsupported);
        return;
    }
    NativeCharacteristic* characteristic = find_characteristic(operation.ticket.filter);
    if (characteristic == nullptr) {
        fail_attribute_read(operation, IoStatus::invalid_argument);
        return;
    }
    characteristic->cccd_handle = operation.pending_cccd_handle;
    characteristic->cccd_value = operation.pending_cccd_value;
    operation.pending_cccd_handle = 0;
    operation.pending_cccd_value = 0;
    operation.phase = NativePhase::subscribe_start;
    wake();
}

void BleNativeState::complete_attribute_read(NativeOperation& operation) noexcept {
    switch (operation.phase) {
    case NativePhase::services_wait:
        complete_primary_discovery(operation);
        return;
    case NativePhase::characteristics_wait:
        complete_characteristic_discovery(operation);
        return;
    case NativePhase::cccd_wait:
        complete_cccd_discovery(operation);
        return;
    default:
        fail_attribute_read(operation, att_status(BT_ATT_ERR_INVALID_PDU));
        return;
    }
}

uint8_t BleNativeState::process_primary_value(NativeOperation& operation,
                                               std::uint16_t handle,
                                               const std::uint8_t* data,
                                               std::uint16_t length) noexcept {
    const std::optional<BleUuid> uuid = portable_uuid(data, length);
    if (!uuid || handle == 0 || handle <= operation.previous_service_handle) {
        fail_attribute_read(operation, att_status(BT_ATT_ERR_INVALID_PDU));
        return BT_GATT_ITER_STOP;
    }
    operation.previous_service_handle = handle;

    if (operation.service_range_open) {
        if (connection.service_count == 0 ||
            connection.services[connection.service_count - 1].start_handle >= handle) {
            fail_attribute_read(operation, att_status(BT_ATT_ERR_INVALID_PDU));
            return BT_GATT_ITER_STOP;
        }
        connection.services[connection.service_count - 1].end_handle =
            static_cast<std::uint16_t>(handle - 1U);
        operation.service_range_open = false;
    }
    if (operation.ticket.id == DeviceOperation::discover &&
        !matches_uuid_filter(*uuid, operation.ticket.filter.service)) {
        return BT_GATT_ITER_CONTINUE;
    }
    if (connection.service_count == connection.services.size()) {
        fail_attribute_read(operation, IoStatus::memory_exhausted);
        return BT_GATT_ITER_STOP;
    }

    connection.services[connection.service_count++] = NativeService{*uuid, handle, 0};
    operation.service_range_open = true;
    return BT_GATT_ITER_CONTINUE;
}

uint8_t BleNativeState::process_characteristic_value(NativeOperation& operation,
                                                      std::uint16_t handle,
                                                      const std::uint8_t* data,
                                                      std::uint16_t length) noexcept {
    constexpr std::uint16_t kShortCharacteristicValueBytes = 5;
    constexpr std::uint16_t kLongCharacteristicValueBytes = 19;
    constexpr std::uint16_t kCharacteristicUuidOffset = 3;
    if ((length != kShortCharacteristicValueBytes &&
         length != kLongCharacteristicValueBytes) ||
        operation.service_index >= connection.service_count) {
        fail_attribute_read(operation, att_status(BT_ATT_ERR_INVALID_PDU));
        return BT_GATT_ITER_STOP;
    }

    const NativeService& service = connection.services[operation.service_index];
    const std::optional<BleUuid> uuid =
        portable_uuid(data + kCharacteristicUuidOffset, length - kCharacteristicUuidOffset);
    const std::uint16_t value_handle = little_u16(data + 1);
    if (!uuid || handle == 0 || handle <= operation.previous_characteristic_handle ||
        handle <= service.start_handle || value_handle <= handle ||
        value_handle > service.end_handle) {
        fail_attribute_read(operation, att_status(BT_ATT_ERR_INVALID_PDU));
        return BT_GATT_ITER_STOP;
    }
    operation.previous_characteristic_handle = handle;

    if (operation.characteristic_range_open) {
        if (connection.characteristic_count == 0) {
            fail_attribute_read(operation, att_status(BT_ATT_ERR_INVALID_PDU));
            return BT_GATT_ITER_STOP;
        }
        NativeCharacteristic& previous =
            connection.characteristics[connection.characteristic_count - 1];
        if (previous.declaration_handle >= handle || previous.value_handle >= handle) {
            fail_attribute_read(operation, att_status(BT_ATT_ERR_INVALID_PDU));
            return BT_GATT_ITER_STOP;
        }
        previous.range_end = static_cast<std::uint16_t>(handle - 1U);
        operation.characteristic_range_open = false;
    }
    if (operation.ticket.id == DeviceOperation::discover &&
        !matches_uuid_filter(*uuid, operation.ticket.filter.characteristic)) {
        return BT_GATT_ITER_CONTINUE;
    }
    if (connection.characteristic_count == connection.characteristics.size()) {
        fail_attribute_read(operation, IoStatus::memory_exhausted);
        return BT_GATT_ITER_STOP;
    }

    NativeCharacteristic characteristic{};
    characteristic.attribute.service = service.uuid;
    characteristic.attribute.characteristic = *uuid;
    characteristic.declaration_handle = handle;
    characteristic.value_handle = value_handle;
    characteristic.range_end = service.end_handle;
    characteristic.properties = data[0];
    connection.characteristics[connection.characteristic_count++] = characteristic;
    operation.characteristic_range_open = true;

    if (operation.ticket.id == DeviceOperation::discover &&
        operation.discovery_result_count < operation.ticket.maximum_results) {
        if (operation.discovery_result_count == operation.results.size()) {
            fail_attribute_read(operation, IoStatus::memory_exhausted);
            return BT_GATT_ITER_STOP;
        }
        operation.results[operation.discovery_result_count++] = BleCharacteristicDescriptor{
            characteristic.attribute,
            (characteristic.properties & BT_GATT_CHRC_READ) != 0,
            (characteristic.properties &
             (BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP)) != 0,
            (characteristic.properties &
             (BT_GATT_CHRC_NOTIFY | BT_GATT_CHRC_INDICATE)) != 0};
    }
    return BT_GATT_ITER_CONTINUE;
}

uint8_t BleNativeState::process_cccd_value(NativeOperation& operation,
                                            std::uint16_t handle, const std::uint8_t* data,
                                            std::uint16_t length) noexcept {
    NativeCharacteristic* characteristic = find_characteristic(operation.ticket.filter);
    if (characteristic == nullptr) {
        fail_attribute_read(operation, IoStatus::invalid_argument);
        return BT_GATT_ITER_STOP;
    }
    if (length != sizeof(std::uint16_t) || handle == 0 ||
        operation.pending_cccd_handle != 0 || handle <= characteristic->value_handle ||
        handle > characteristic->range_end) {
        fail_attribute_read(operation, att_status(BT_ATT_ERR_INVALID_PDU));
        return BT_GATT_ITER_STOP;
    }
    operation.pending_cccd_handle = handle;
    operation.pending_cccd_value = little_u16(data);
    return BT_GATT_ITER_CONTINUE;
}

uint8_t BleNativeState::on_attribute_read(
    bt_conn* conn, std::uint8_t error, bt_gatt_read_params* parameters,
    const void* data, std::uint16_t length) noexcept {
    NativeCallbackGuard callback_guard;
    if (parameters == nullptr) return BT_GATT_ITER_STOP;
    NativeOperation& operation = *CONTAINER_OF(parameters, NativeOperation, read_parameters);
    BleNativeState* self = operation.owner;
    if (self == nullptr) return BT_GATT_ITER_STOP;

    bool valid = false;
    bool cancelled = false;
    NativePhase phase = NativePhase::none;
    {
        const auto key = k_spin_lock(&self->lock);
        phase = operation.phase;
        valid = self->current_is(operation) && conn == self->connection.conn &&
                self->connected(operation) &&
                (phase == NativePhase::services_wait ||
                 phase == NativePhase::characteristics_wait || phase == NativePhase::cccd_wait);
        cancelled = operation.cancellation_requested || operation.retirement_requested;
        k_spin_unlock(&self->lock, key);
    }
    if (!valid) return BT_GATT_ITER_STOP;
    if (cancelled) {
        self->fail_attribute_read(operation, IoStatus::cancelled);
        return BT_GATT_ITER_STOP;
    }

    if (error != BT_ATT_ERR_SUCCESS) {
        if (error == BT_ATT_ERR_ATTRIBUTE_NOT_FOUND && data == nullptr) {
            self->complete_attribute_read(operation);
        } else {
            self->fail_attribute_read(operation, att_status(error));
        }
        return BT_GATT_ITER_STOP;
    }
    if (data == nullptr) {
        if (operation.last_attribute_handle == 0 ||
            parameters->by_uuid.start_handle != operation.last_attribute_handle ||
            parameters->by_uuid.start_handle != parameters->by_uuid.end_handle) {
            self->fail_attribute_read(operation, att_status(BT_ATT_ERR_INVALID_PDU));
        } else {
            self->complete_attribute_read(operation);
        }
        return BT_GATT_ITER_STOP;
    }

    const std::uint16_t handle = parameters->by_uuid.start_handle;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    uint8_t iterator = BT_GATT_ITER_STOP;
    switch (phase) {
    case NativePhase::services_wait:
        iterator = self->process_primary_value(operation, handle, bytes, length);
        break;
    case NativePhase::characteristics_wait:
        iterator = self->process_characteristic_value(operation, handle, bytes, length);
        break;
    case NativePhase::cccd_wait:
        iterator = self->process_cccd_value(operation, handle, bytes, length);
        break;
    default:
        self->fail_attribute_read(operation, att_status(BT_ATT_ERR_INVALID_PDU));
        return BT_GATT_ITER_STOP;
    }
    if (iterator == BT_GATT_ITER_CONTINUE) operation.last_attribute_handle = handle;
    return iterator;
}

uint8_t BleNativeState::on_read(
    bt_conn* /*conn*/, std::uint8_t error, bt_gatt_read_params* parameters,
    const void* data, std::uint16_t length) noexcept {
    NativeCallbackGuard callback_guard;
    if (parameters == nullptr) return BT_GATT_ITER_STOP;
    NativeOperation& operation = *CONTAINER_OF(parameters, NativeOperation, read_parameters);
    BleNativeState* self = operation.owner;
    if (self == nullptr || !self->current_is(operation)) return BT_GATT_ITER_STOP;
    if (error != BT_ATT_ERR_SUCCESS) {
        self->finish(operation, operation.cancellation_requested ? IoStatus::cancelled
                                                                 : att_status(error));
        return BT_GATT_ITER_STOP;
    }
    if (data == nullptr) {
        self->finish(operation, operation.cancellation_requested ? IoStatus::cancelled
                                                                 : IoStatus::completed);
        return BT_GATT_ITER_STOP;
    }
    const std::size_t allowed = std::min<std::size_t>(
        operation.value.size(), operation.ticket.maximum_bytes);
    if (length > allowed - std::min<std::size_t>(allowed, operation.result_value_length)) {
        self->finish(operation, IoStatus::memory_exhausted);
        return BT_GATT_ITER_STOP;
    }
    if (operation.result_value_length > allowed) {
        self->finish(operation, IoStatus::memory_exhausted);
        return BT_GATT_ITER_STOP;
    }
    if (length != 0) {
        std::memcpy(operation.value.data() + operation.result_value_length, data, length);
    }
    operation.result_value_length = static_cast<std::uint16_t>(
        operation.result_value_length + length);
    return BT_GATT_ITER_CONTINUE;
}

void BleNativeState::on_written(
    bt_conn* /*conn*/, std::uint8_t error,
    bt_gatt_write_params* parameters) noexcept {
    NativeCallbackGuard callback_guard;
    if (parameters == nullptr) return;
    NativeOperation& operation = *CONTAINER_OF(parameters, NativeOperation, write_parameters);
    BleNativeState* self = operation.owner;
    if (self == nullptr || !self->current_is(operation)) return;
    operation.result_value_length = error == BT_ATT_ERR_SUCCESS ? operation.ticket.value_length : 0;
    self->finish(operation, operation.cancellation_requested ? IoStatus::cancelled
                                                             : att_status(error));
}

void BleNativeState::on_write_command_complete(bt_conn* conn, void* user_data) noexcept {
    (void)conn;
    NativeCallbackGuard callback_guard;
    if (user_data == nullptr) return;
    NativeOperation& operation = *static_cast<NativeOperation*>(user_data);
    BleNativeState* self = operation.owner;
    if (self == nullptr || !self->current_is(operation)) return;
    operation.result_value_length = operation.ticket.value_length;
    self->finish(operation, operation.cancellation_requested ? IoStatus::cancelled
                                                             : IoStatus::completed);
}

uint8_t BleNativeState::on_notified(
    bt_conn* /*conn*/, bt_gatt_subscribe_params* parameters,
    const void* data, std::uint16_t length) noexcept {
    NativeCallbackGuard callback_guard;
    if (parameters == nullptr) return BT_GATT_ITER_STOP;
    NativeSubscription& subscription =
        *CONTAINER_OF(parameters, NativeSubscription, parameters);
    BleNativeState* self = subscription.owner;
    if (self == nullptr) return BT_GATT_ITER_STOP;

    if (data == nullptr) {
        const auto key = k_spin_lock(&self->lock);
        subscription.sdk_linked = false;
        subscription.accept_events = false;
        for (NativeOperation& operation : self->operations) {
            if (operation.state != NativeWorkState::running ||
                operation.subscription_index == kNoIndex ||
                &self->subscriptions[operation.subscription_index] != &subscription) {
                continue;
            }
            if (operation.phase == NativePhase::unsubscribe_wait ||
                operation.phase == NativePhase::subscribe_wait) {
                operation.terminal_status = operation.cancellation_requested
                                                ? IoStatus::cancelled
                                                : (operation.phase == NativePhase::unsubscribe_wait
                                                       ? operation.terminal_status
                                                       : IoStatus::device_lost);
                operation.state = NativeWorkState::finishing;
                operation.phase = NativePhase::none;
            }
        }
        k_spin_unlock(&self->lock, key);
        self->wake();
        return BT_GATT_ITER_STOP;
    }

    const auto key = k_spin_lock(&self->lock);
    if (!subscription.reserved || !subscription.sdk_linked || !subscription.accept_events ||
        subscription.target != BleSubscriptionTarget::characteristic ||
        length > zephyr_ble_central_max_attribute_bytes) {
        k_spin_unlock(&self->lock, key);
        return BT_GATT_ITER_CONTINUE;
    }
    if (self->notification_count == self->notifications.size()) {
        k_spin_unlock(&self->lock, key);
        return BT_GATT_ITER_CONTINUE;
    }
    const std::size_t tail =
        (self->notification_head + self->notification_count) % self->notifications.size();
    NativeNotification& event = self->notifications[tail];
    event.subscription = subscription.subscription;
    event.device = subscription.device;
    event.owner_generation = subscription.owner_generation;
    event.connection_generation = subscription.connection_generation;
    event.target = subscription.target;
    event.attribute = subscription.attribute;
    event.value_length = length;
    if (length != 0) std::memcpy(event.value.data(), data, length);
    ++self->notification_count;
    k_spin_unlock(&self->lock, key);
    return BT_GATT_ITER_CONTINUE;
}

void BleNativeState::on_subscribed(
    bt_conn* /*conn*/, std::uint8_t error,
    bt_gatt_subscribe_params* parameters) noexcept {
    NativeCallbackGuard callback_guard;
    if (parameters == nullptr) return;
    NativeSubscription& subscription =
        *CONTAINER_OF(parameters, NativeSubscription, parameters);
    BleNativeState* self = subscription.owner;
    if (self == nullptr) return;
    const auto key = k_spin_lock(&self->lock);
    if (self->active_operation == kNoIndex) {
        k_spin_unlock(&self->lock, key);
        return;
    }
    NativeOperation& operation = self->current_operation();
    if (operation.subscription_index == kNoIndex ||
        &self->subscriptions[operation.subscription_index] != &subscription ||
        operation.state != NativeWorkState::running) {
        k_spin_unlock(&self->lock, key);
        return;
    }
    if (parameters->value == 0) {
        operation.terminal_status = operation.cancellation_requested
                                        ? IoStatus::cancelled
                                        : (error == BT_ATT_ERR_SUCCESS ? IoStatus::completed
                                                                       : att_status(error));
        operation.phase = NativePhase::none;
        operation.state = NativeWorkState::finishing;
    } else if (error != BT_ATT_ERR_SUCCESS) {
        operation.terminal_status = operation.cancellation_requested ? IoStatus::cancelled
                                                                     : att_status(error);
        operation.phase = NativePhase::none;
        operation.state = NativeWorkState::finishing;
    } else if (operation.cancellation_requested || operation.retirement_requested) {
        subscription.accept_events = false;
        operation.phase = NativePhase::unsubscribe_start;
    } else {
        subscription.accept_events = true;
        operation.response_connection_generation = subscription.connection_generation;
        operation.terminal_status = IoStatus::completed;
        operation.phase = NativePhase::none;
        operation.state = NativeWorkState::finishing;
    }
    k_spin_unlock(&self->lock, key);
    self->wake();
}

} // namespace
} // namespace mb::zephyr

namespace mb::zephyr {

struct ZephyrBleCentralBackend::State {
    struct UiOperation {
        BackendOperation operation{};
        std::weak_ptr<PeripheralCompletionSink> completions{};
        std::uint32_t sequence = 0;
        bool reserved = false;
    };

    struct UiSubscription {
        DeviceHandle subscription{};
        DeviceHandle device{};
        std::uint64_t owner_generation = 0;
        std::uint32_t connection_generation = 0;
        BleSubscriptionTarget target = BleSubscriptionTarget::characteristic;
        BleAttribute attribute{};
        std::weak_ptr<PeripheralCompletionSink> completions{};
        bool active = false;
    };

    explicit State(const ZephyrBleCentralConfig& config)
        : application_thread(k_current_get()), native(config.scan_timeout_ms) {
        if (config.logical_name.empty() ||
            config.logical_name.size() > zephyr_ble_central_max_logical_name_bytes) {
            throw std::invalid_argument("invalid Zephyr BLE central logical name");
        }
        if (config.scan_timeout_ms == 0 ||
            config.scan_timeout_ms > static_cast<std::uint32_t>(std::numeric_limits<int32_t>::max())) {
            throw std::invalid_argument("Zephyr BLE central scan timeout must be finite");
        }
        if (k_is_in_isr() || k_current_get() == k_work_queue_thread_get(&k_sys_work_q)) {
            throw std::invalid_argument("Zephyr BLE central requires a non-workqueue application thread");
        }
        std::copy(config.logical_name.begin(), config.logical_name.end(), logical_name.begin());
        logical_name_size = config.logical_name.size();
        if (!bt_is_ready()) {
            const int error = bt_enable(nullptr);
            if (error != 0) {
                throw std::runtime_error("bt_enable failed: " + std::to_string(error));
            }
        }
        if (!bt_is_ready()) throw std::runtime_error("Bluetooth did not become ready");
        if (start_native_ble_work_queue() != IoStatus::completed) {
            throw std::runtime_error("could not start the BLE native work queue");
        }
        const IoStatus registered = register_native_state(&native);
        if (registered != IoStatus::completed) {
            throw std::runtime_error("could not register Zephyr BLE central connection callbacks");
        }
    }

    ~State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;

    void assert_application_thread() const noexcept {
        if (k_is_in_isr() || k_current_get() != application_thread) k_oops();
    }

    [[nodiscard]] std::string_view name() const noexcept {
        return {logical_name.data(), logical_name_size};
    }

    [[nodiscard]] IoStatus validate_open(const BackendOperation& operation) const noexcept {
        const auto& request = std::get<OpenRequest>(operation.request);
        if (request.minimum_version.capability != Capability::ble_central) {
            return IoStatus::invalid_argument;
        }
        if (request.minimum_version.major != peripheral_abi_major ||
            request.minimum_version.minor != zephyr_ble_central_version.minor) {
            return IoStatus::unsupported;
        }
        if (!operation.device.is_empty()) return IoStatus::invalid_argument;
        return lease_open || lease_pending || native.release_cleanup || native.close_cleanup
                       ? IoStatus::busy
                       : IoStatus::completed;
    }

    [[nodiscard]] IoStatus validate_request(const BackendOperation& operation) const noexcept {
        if (operation.logical_name != name() || operation.owner_generation == 0 ||
            operation.budget.maximum_open_handles == 0 ||
            operation.budget.maximum_pending_operations == 0 ||
            operation.budget.maximum_pending_operations > zephyr_ble_central_max_pending_operations ||
            operation.budget.maximum_bytes_per_buffer == 0 || !request_matches(operation) ||
            operation.id == DeviceOperation::cancel) {
            return IoStatus::invalid_argument;
        }
        if (native.shutting_down) return IoStatus::device_lost;
        if (operation.id == DeviceOperation::open) return validate_open(operation);
        if (!lease_open || native.close_cleanup || operation.device.is_empty() ||
            operation.device.capability != Capability::ble_central ||
            operation.device.owner_generation != operation.owner_generation ||
            operation.owner_generation != lease_owner ||
            (!lease_device.is_empty() && operation.device != lease_device)) {
            return IoStatus::invalid_argument;
        }

        switch (operation.id) {
        case DeviceOperation::discover: {
            const auto& request = std::get<BleDiscoverRequest>(operation.request);
            if (request.maximum_results == 0) return IoStatus::invalid_argument;
            if (request.kind == BleDiscoveryKind::scan) {
                if (!wildcard_attribute(request.filter)) return IoStatus::invalid_argument;
                const auto key = k_spin_lock(&g_scan.lock);
                const bool occupied = g_scan.owner != nullptr;
                k_spin_unlock(&g_scan.lock, key);
                return occupied ? IoStatus::busy : IoStatus::completed;
            }
            if (request.kind != BleDiscoveryKind::services &&
                request.kind != BleDiscoveryKind::characteristics) {
                return IoStatus::invalid_argument;
            }
            return valid_attribute(request.filter, true)
                       ? (native.connection.connected ? IoStatus::completed : IoStatus::busy)
                       : IoStatus::invalid_argument;
        }
        case DeviceOperation::connect: {
            const auto& request = std::get<BleConnectRequest>(operation.request);
            if (request.supervision_timeout_ms < kMinimumSupervisionTimeoutMs ||
                request.supervision_timeout_ms > kMaximumSupervisionTimeoutMs ||
                request.supervision_timeout_ms % 10U != 0) {
                return IoStatus::invalid_argument;
            }
            if (std::all_of(request.peer.bytes.begin(), request.peer.bytes.end(),
                            [](std::uint8_t byte) { return byte == 0; })) {
                return IoStatus::invalid_argument;
            }
            if (native.connection.conn != nullptr) return IoStatus::busy;
            const auto peer = std::find_if(discovered_peers.begin(),
                                           discovered_peers.begin() + discovered_peer_count,
                                           [&](const BleAddress& candidate) {
                                               return same_address(candidate, request.peer);
                                           });
            return peer == discovered_peers.begin() + discovered_peer_count
                       ? IoStatus::invalid_argument
                       : IoStatus::completed;
        }
        case DeviceOperation::disconnect: {
            const auto& request = std::get<BleDisconnectRequest>(operation.request);
            return native.connection.connected &&
                           same_address(native.connection.peer, request.peer)
                       ? IoStatus::completed
                       : IoStatus::invalid_argument;
        }
        case DeviceOperation::read:
            return valid_attribute(std::get<BleReadRequest>(operation.request).attribute)
                       ? (native.connection.connected ? IoStatus::completed : IoStatus::busy)
                       : IoStatus::invalid_argument;
        case DeviceOperation::write: {
            const auto& request = std::get<BleWriteRequest>(operation.request);
            if (!valid_attribute(request.attribute) || request.value == nullptr ||
                request.value->empty() ||
                (request.mode != BleWriteMode::with_response &&
                 request.mode != BleWriteMode::without_response)) {
                return IoStatus::invalid_argument;
            }
            if (!native.connection.connected) return IoStatus::busy;
            return request.value->size() <= std::min<std::size_t>(
                       operation.budget.maximum_bytes_per_buffer,
                       zephyr_ble_central_max_attribute_bytes)
                       ? IoStatus::completed
                       : IoStatus::memory_exhausted;
        }
        case DeviceOperation::subscribe: {
            const auto& request = std::get<BleSubscribeRequest>(operation.request);
            if (operation.subscription.is_empty() ||
                operation.subscription.capability != Capability::ble_central ||
                operation.subscription.owner_generation != operation.owner_generation) {
                return IoStatus::invalid_argument;
            }
            if (request.target == BleSubscriptionTarget::connection_state) {
                return wildcard_attribute(request.attribute) ? IoStatus::completed
                                                              : IoStatus::invalid_argument;
            }
            if (request.target != BleSubscriptionTarget::characteristic ||
                !valid_attribute(request.attribute)) {
                return IoStatus::invalid_argument;
            }
            if (!native.connection.connected) return IoStatus::busy;
            return operation.budget.maximum_bytes_per_buffer >= zephyr_ble_central_max_attribute_bytes
                       ? IoStatus::completed
                       : IoStatus::memory_exhausted;
        }
        case DeviceOperation::unsubscribe:
            return std::get<UnsubscribeRequest>(operation.request).subscription.is_empty()
                       ? IoStatus::invalid_argument
                       : IoStatus::completed;
        case DeviceOperation::close: return IoStatus::completed;
        default: return IoStatus::unsupported;
        }
    }

    [[nodiscard]] NativeTicket ticket_from(const BackendOperation& operation) const noexcept {
        NativeTicket ticket;
        ticket.device = operation.device;
        ticket.operation = operation.operation;
        ticket.subscription = operation.subscription;
        ticket.owner_generation = operation.owner_generation;
        ticket.id = operation.id;
        ticket.maximum_bytes = operation.budget.maximum_bytes_per_buffer;
        if (const auto* discover = std::get_if<BleDiscoverRequest>(&operation.request)) {
            ticket.discovery_kind = discover->kind;
            ticket.filter = discover->filter;
            ticket.maximum_results = discover->maximum_results;
        } else if (const auto* connect = std::get_if<BleConnectRequest>(&operation.request)) {
            ticket.peer = connect->peer;
            ticket.supervision_timeout_ms = connect->supervision_timeout_ms;
        } else if (const auto* disconnect = std::get_if<BleDisconnectRequest>(&operation.request)) {
            ticket.peer = disconnect->peer;
        } else if (const auto* read = std::get_if<BleReadRequest>(&operation.request)) {
            ticket.filter = read->attribute;
        } else if (const auto* subscribe = std::get_if<BleSubscribeRequest>(&operation.request)) {
            ticket.filter = subscribe->attribute;
            ticket.subscription_target = subscribe->target;
        } else if (const auto* unsubscribe = std::get_if<UnsubscribeRequest>(&operation.request)) {
            ticket.subscription = unsubscribe->subscription;
        }
        return ticket;
    }

    [[nodiscard]] std::uint8_t reserve_subscription(const BackendOperation& operation) noexcept {
        std::uint8_t free = kNoIndex;
        const auto& request = std::get<BleSubscribeRequest>(operation.request);
        for (std::size_t index = 0; index < native.subscriptions.size(); ++index) {
            NativeSubscription& subscription = native.subscriptions[index];
            if (request.target == BleSubscriptionTarget::characteristic && subscription.reserved &&
                subscription.target == request.target &&
                subscription.connection_generation == native.connection.generation &&
                same_attribute(subscription.attribute, request.attribute)) {
                return kNoIndex;
            }
            if (!subscription.reserved && free == kNoIndex) free = static_cast<std::uint8_t>(index);
        }
        if (free != kNoIndex) {
            NativeSubscription& subscription = native.subscriptions[free];
            subscription.subscription = operation.subscription;
            subscription.device = operation.device;
            subscription.owner_generation = operation.owner_generation;
            subscription.target = request.target;
            subscription.attribute = request.attribute;
            subscription.connection_generation = native.connection.generation;
            subscription.value_handle = 0;
            subscription.reserved = true;
            subscription.sdk_linked = false;
            subscription.accept_events = false;
            subscription.parameters = {};
            ui_subscriptions[free] = UiSubscription{
                operation.subscription, operation.device, operation.owner_generation,
                native.connection.generation, request.target, request.attribute, {}, false};
        }
        return free;
    }

    [[nodiscard]] std::uint8_t find_unsubscription(const BackendOperation& operation) noexcept {
        const DeviceHandle handle = std::get<UnsubscribeRequest>(operation.request).subscription;
        for (std::size_t index = 0; index < native.subscriptions.size(); ++index) {
            const NativeSubscription& subscription = native.subscriptions[index];
            if (subscription.reserved && subscription.subscription == handle &&
                subscription.device == operation.device &&
                subscription.owner_generation == operation.owner_generation) {
                return static_cast<std::uint8_t>(index);
            }
        }
        return kNoIndex;
    }

    void clear_ui_subscription(std::size_t index) noexcept {
        if (index < ui_subscriptions.size()) ui_subscriptions[index] = {};
    }

    void suppress_device(const BackendOperation& control, std::size_t accepted_index) noexcept {
        for (std::size_t index = 0; index < native.operations.size(); ++index) {
            if (index == accepted_index) continue;
            NativeOperation& candidate = native.operations[index];
            if (candidate.state != NativeWorkState::idle &&
                candidate.ticket.owner_generation == control.owner_generation &&
                (candidate.ticket.device == control.device || candidate.ticket.device.is_empty())) {
                candidate.cancellation_requested = true;
                candidate.retirement_requested = true;
            }
        }
        for (std::size_t index = 0; index < native.subscriptions.size(); ++index) {
            NativeSubscription& subscription = native.subscriptions[index];
            if (subscription.reserved && subscription.device == control.device &&
                subscription.owner_generation == control.owner_generation) {
                subscription.accept_events = false;
                clear_ui_subscription(index);
            }
        }
    }

    IoStatus start(const BackendOperation& operation,
                   std::weak_ptr<PeripheralCompletionSink> completions) noexcept {
        const auto key = k_spin_lock(&native.lock);
        const IoStatus validation = validate_request(operation);
        if (validation != IoStatus::completed) {
            k_spin_unlock(&native.lock, key);
            return validation;
        }
        NativeTicket ticket = ticket_from(operation);
        if (operation.id == DeviceOperation::write) {
            const auto& write = std::get<BleWriteRequest>(operation.request);
            ticket.write_mode = write.mode;
            ticket.value_length = static_cast<std::uint16_t>(write.value->size());
        }
        std::size_t index = 0;
        while (index < native.operations.size() &&
               native.operations[index].state != NativeWorkState::idle) {
            ++index;
        }
        if (index == native.operations.size()) {
            k_spin_unlock(&native.lock, key);
            return IoStatus::busy;
        }
        NativeOperation& native_operation = native.operations[index];
        std::uint8_t subscription_index = kNoIndex;
        if (operation.id == DeviceOperation::subscribe) {
            subscription_index = reserve_subscription(operation);
            if (subscription_index == kNoIndex) {
                k_spin_unlock(&native.lock, key);
                return IoStatus::busy;
            }
        } else if (operation.id == DeviceOperation::unsubscribe) {
            subscription_index = find_unsubscription(operation);
            if (subscription_index == kNoIndex) {
                k_spin_unlock(&native.lock, key);
                return IoStatus::invalid_argument;
            }
            native.subscriptions[subscription_index].accept_events = false;
            clear_ui_subscription(subscription_index);
        }

        native_operation = {};
        native_operation.owner = &native;
        native_operation.ticket = ticket;
        native_operation.sequence = next_sequence++;
        if (next_sequence == 0) ++next_sequence;
        native_operation.connection_generation = native.connection.generation;
        native_operation.subscription_index = subscription_index;
        native_operation.state = NativeWorkState::queued;
        if (operation.id == DeviceOperation::write) {
            const auto& write = std::get<BleWriteRequest>(operation.request);
            std::copy(write.value->begin(), write.value->end(), native_operation.value.begin());
        }
        native.operation_queue[(native.operation_head + native.operation_count) %
                               native.operation_queue.size()] = static_cast<std::uint8_t>(index);
        ++native.operation_count;
        if (operation.id == DeviceOperation::open) native.lease_owner = operation.owner_generation;
        if (operation.id == DeviceOperation::close) {
            native.close_cleanup = true;
            native.connection.disconnect_cleanup = true;
            suppress_device(operation, index);
        }
        k_spin_unlock(&native.lock, key);

        ui_operations[index].operation = operation;
        ui_operations[index].completions = std::move(completions);
        ui_operations[index].sequence = native_operation.sequence;
        ui_operations[index].reserved = true;
        if (operation.id == DeviceOperation::subscribe) {
            ui_subscriptions[subscription_index].completions = ui_operations[index].completions;
        }
        if (operation.id == DeviceOperation::open) {
            lease_pending = true;
            lease_owner = operation.owner_generation;
            lease_device = {};
        } else if (lease_device.is_empty()) {
            lease_device = operation.device;
        }
        if (operation.id == DeviceOperation::close) {
            for (std::size_t sub = 0; sub < ui_subscriptions.size(); ++sub) {
                clear_ui_subscription(sub);
            }
        }
        native.wake();
        return IoStatus::completed;
    }

    IoStatus cancel(const BackendOperation& operation) noexcept {
        const auto key = k_spin_lock(&native.lock);
        for (NativeOperation& candidate : native.operations) {
            if ((candidate.state != NativeWorkState::queued &&
                 candidate.state != NativeWorkState::running) ||
                candidate.ticket.operation != operation.operation ||
                candidate.ticket.owner_generation != operation.owner_generation) {
                continue;
            }
            if (candidate.cancellation_requested || candidate.retirement_requested) {
                k_spin_unlock(&native.lock, key);
                return IoStatus::invalid_argument;
            }
            candidate.cancellation_requested = true;
            if (candidate.ticket.id == DeviceOperation::subscribe &&
                candidate.subscription_index != kNoIndex) {
                native.subscriptions[candidate.subscription_index].accept_events = false;
            }
            k_spin_unlock(&native.lock, key);
            native.wake();
            return IoStatus::completed;
        }
        k_spin_unlock(&native.lock, key);
        return IoStatus::invalid_argument;
    }

    void release(std::uint64_t owner_generation) noexcept {
        const auto key = k_spin_lock(&native.lock);
        if (owner_generation != lease_owner && owner_generation != native.lease_owner) {
            k_spin_unlock(&native.lock, key);
            return;
        }
        native.connection.disconnect_failed = false;
        native.connection.disconnect_failure = IoStatus::completed;
        native.release_cleanup = true;
        native.close_cleanup = true;
        native.connection.disconnect_cleanup = true;
        for (NativeOperation& operation : native.operations) {
            if (operation.state != NativeWorkState::idle &&
                operation.ticket.owner_generation == owner_generation) {
                operation.cancellation_requested = true;
                operation.retirement_requested = true;
            }
        }
        for (std::size_t index = 0; index < native.subscriptions.size(); ++index) {
            NativeSubscription& subscription = native.subscriptions[index];
            if (subscription.reserved && subscription.owner_generation == owner_generation) {
                subscription.accept_events = false;
                if (subscription.target == BleSubscriptionTarget::connection_state) {
                    subscription.reserved = false;
                }
                clear_ui_subscription(index);
            }
        }
        lease_open = false;
        lease_pending = false;
        lease_device = {};
        k_spin_unlock(&native.lock, key);
        discovered_peer_count = 0;
        native.wake();
    }

    [[nodiscard]] DeviceResponse response_for(NativeOperation& operation, IoStatus& status) {
        if (status != IoStatus::completed) return std::monostate{};
        switch (operation.ticket.id) {
        case DeviceOperation::open:
            return OpenResponse{{}, zephyr_ble_central_version, false};
        case DeviceOperation::discover:
            if (operation.ticket.discovery_kind == BleDiscoveryKind::scan) {
                BleScanResponse response;
                const auto key = k_spin_lock(&g_scan.lock);
                const bool valid = g_scan.owner == &native && g_scan.operation == &operation &&
                                   !g_scan.scanning;
                const std::uint16_t count = valid ? g_scan.peer_count : 0;
                k_spin_unlock(&g_scan.lock, key);
                if (!valid) {
                    status = IoStatus::io_failure;
                    return std::monostate{};
                }
                response.peers.reserve(count);
                for (std::uint16_t index = 0; index < count; ++index) {
                    const auto locked = k_spin_lock(&g_scan.lock);
                    scan_delivery_record = g_scan.peers[index];
                    k_spin_unlock(&g_scan.lock, locked);
                    response.peers.push_back(BlePeer{
                        scan_delivery_record.address, scan_delivery_record.rssi,
                        make_immutable_bytes(std::span<const std::uint8_t>(
                            scan_delivery_record.advertisement.data(),
                            scan_delivery_record.advertisement_length))});
                }
                release_scan_owner(&native, &operation);
                discovered_peer_count = 0;
                for (const BlePeer& peer : response.peers) {
                    if (discovered_peer_count == discovered_peers.size()) break;
                    discovered_peers[discovered_peer_count++] = peer.address;
                }
                return response;
            }
            {
                BleGattDiscoveryResponse response;
                response.characteristics.reserve(operation.discovery_result_count);
                for (std::uint16_t index = 0; index < operation.discovery_result_count; ++index) {
                    response.characteristics.push_back(operation.results[index]);
                }
                return response;
            }
        case DeviceOperation::connect:
            return BleConnectionResponse{operation.response_peer, operation.response_connected,
                                         operation.response_paired};
        case DeviceOperation::read:
            return BleReadResponse{operation.ticket.filter,
                                   make_immutable_bytes(std::span<const std::uint8_t>(
                                       operation.value.data(), operation.result_value_length))};
        case DeviceOperation::write:
            return BleWriteResponse{operation.result_value_length};
        case DeviceOperation::subscribe:
            return SubscriptionResponse{operation.ticket.subscription};
        case DeviceOperation::disconnect:
        case DeviceOperation::unsubscribe:
        case DeviceOperation::close:
            return EmptyResponse{};
        default:
            status = IoStatus::unsupported;
            return std::monostate{};
        }
    }

    void complete_results() {
        for (std::size_t count = 0; count < zephyr_ble_central_max_pending_operations; ++count) {
            std::uint8_t index = kNoIndex;
            {
                const auto key = k_spin_lock(&native.lock);
                if (native.result_count == 0) {
                    k_spin_unlock(&native.lock, key);
                    return;
                }
                index = native.result_queue[native.result_head];
                native.result_head = (native.result_head + 1) % native.result_queue.size();
                --native.result_count;
                k_spin_unlock(&native.lock, key);
            }
            NativeOperation& operation = native.operations[index];
            UiOperation& ui = ui_operations[index];
            if (!ui.reserved || operation.state != NativeWorkState::result_ready ||
                ui.sequence != operation.sequence) {
                operation = {};
                operation.owner = &native;
                continue;
            }
            IoStatus status = operation.cancellation_requested ? IoStatus::cancelled
                                                               : operation.terminal_status;
            const bool retired = operation.retirement_requested || native.shutting_down;
            const DeviceOperation id = operation.ticket.id;
            const std::uint8_t subscription_index = operation.subscription_index;
            DeviceResponse response = response_for(operation, status);
            if (id == DeviceOperation::discover &&
                operation.ticket.discovery_kind == BleDiscoveryKind::scan) {
                release_scan_owner(&native, &operation);
            }

            if (id == DeviceOperation::open) {
                lease_pending = false;
                lease_open = status == IoStatus::completed;
                if (!lease_open) lease_device = {};
            } else if (id == DeviceOperation::close) {
                lease_open = false;
                lease_device = {};
                discovered_peer_count = 0;
            } else if (id == DeviceOperation::subscribe && subscription_index != kNoIndex) {
                if (status == IoStatus::completed) {
                    UiSubscription& subscription = ui_subscriptions[subscription_index];
                    subscription.active = true;
                    subscription.connection_generation = operation.response_connection_generation;
                    if (operation.ticket.subscription_target ==
                        BleSubscriptionTarget::connection_state) {
                        subscription.connection_generation = 0;
                    }
                } else {
                    clear_ui_subscription(subscription_index);
                    if (!native.subscriptions[subscription_index].sdk_linked) {
                        native.subscriptions[subscription_index].reserved = false;
                    }
                }
            } else if (id == DeviceOperation::unsubscribe && subscription_index != kNoIndex) {
                clear_ui_subscription(subscription_index);
                if (!native.subscriptions[subscription_index].sdk_linked) {
                    native.subscriptions[subscription_index].reserved = false;
                }
            }

            BackendOperation backend_operation = std::move(ui.operation);
            auto completion_reference = std::move(ui.completions);
            ui = {};
            operation = {};
            operation.owner = &native;
            if (!retired) {
                const auto completion = completion_reference.lock();
                if (completion) completion->complete(backend_operation, status, std::move(response));
            }
        }
    }

    void deliver_notifications() {
        for (std::size_t count = 0; count < zephyr_ble_central_max_notifications; ++count) {
            NativeNotification event{};
            {
                const auto key = k_spin_lock(&native.lock);
                if (native.notification_count == 0) {
                    k_spin_unlock(&native.lock, key);
                    return;
                }
                event = native.notifications[native.notification_head];
                native.notification_head =
                    (native.notification_head + 1) % native.notifications.size();
                --native.notification_count;
                k_spin_unlock(&native.lock, key);
            }
            std::size_t index = 0;
            while (index < ui_subscriptions.size()) {
                const UiSubscription& subscription = ui_subscriptions[index];
                if (subscription.active && subscription.subscription == event.subscription &&
                    subscription.device == event.device &&
                    subscription.owner_generation == event.owner_generation &&
                    subscription.target == event.target &&
                    (subscription.target == BleSubscriptionTarget::connection_state ||
                     (subscription.connection_generation == event.connection_generation &&
                      same_attribute(subscription.attribute, event.attribute)))) {
                    break;
                }
                ++index;
            }
            if (index == ui_subscriptions.size()) continue;
            const auto completion = ui_subscriptions[index].completions.lock();
            if (!completion) {
                clear_ui_subscription(index);
                continue;
            }
            DeviceResponse response;
            if (event.target == BleSubscriptionTarget::connection_state) {
                response = BleConnectionResponse{event.peer, event.connected, event.paired};
            } else {
                response = BleReadResponse{
                    event.attribute,
                    make_immutable_bytes(std::span<const std::uint8_t>(event.value.data(),
                                                                      event.value_length))};
            }
            const IoStatus status = completion->publish(
                event.owner_generation, event.subscription, std::move(response));
            if (status == IoStatus::completed || status == IoStatus::busy ||
                status == IoStatus::memory_exhausted) {
                continue;
            }
            clear_ui_subscription(index);
            const auto key = k_spin_lock(&native.lock);
            native.subscriptions[index].accept_events = false;
            k_spin_unlock(&native.lock, key);
        }
    }

    void poll() {
        complete_results();
        if (!native.shutting_down) deliver_notifications();
    }

    IoStatus shutdown() noexcept {
        if (native.shutdown_complete) return IoStatus::completed;
        {
            const auto key = k_spin_lock(&native.lock);
            native.connection.disconnect_failed = false;
            native.connection.disconnect_failure = IoStatus::completed;
            native.shutting_down = true;
            native.close_cleanup = true;
            native.release_cleanup = true;
            native.connection.disconnect_cleanup = true;
            for (NativeOperation& operation : native.operations) {
                if (operation.state != NativeWorkState::idle) {
                    operation.cancellation_requested = true;
                    operation.retirement_requested = true;
                }
            }
            for (NativeSubscription& subscription : native.subscriptions) {
                subscription.accept_events = false;
            }
            k_spin_unlock(&native.lock, key);
            k_work_sync scan_sync;
            (void)k_work_cancel_delayable_sync(&native.scan_timer, &scan_sync);
            const auto scan_key = k_spin_lock(&g_scan.lock);
            if (g_scan.owner == &native && g_scan.scanning) g_scan.stop_requested = true;
            k_spin_unlock(&g_scan.lock, scan_key);
        }
        native.wake();

        const std::int64_t deadline = k_uptime_get() + zephyr_ble_central_shutdown_timeout_ms;
        for (;;) {
            complete_results();
            {
                const auto key = k_spin_lock(&native.lock);
                native.notification_head = 0;
                native.notification_count = 0;
                k_spin_unlock(&native.lock, key);
            }
            const int cancel_state = k_work_cancel(&native.work_item);
            const int busy_state = k_work_busy_get(&native.work_item);
            k_work_sync scan_sync;
            (void)k_work_cancel_delayable_sync(&native.scan_timer, &scan_sync);
            const bool worker_busy =
                ((cancel_state | busy_state) &
                 (K_WORK_RUNNING | K_WORK_QUEUED | K_WORK_CANCELING)) != 0;
            const auto key = k_spin_lock(&native.lock);
            const bool idle = native.idle_for_destruction();
            k_spin_unlock(&native.lock, key);
            const auto scan_key = k_spin_lock(&g_scan.lock);
            const bool scan_stopped = !g_scan.scanning || g_scan.owner != &native;
            k_spin_unlock(&g_scan.lock, scan_key);
            if (idle && scan_stopped && !worker_busy) {
                release_scan_owner(&native, nullptr);
                unregister_native_state(&native);
                native.shutdown_complete = true;
                for (UiOperation& operation : ui_operations) operation = {};
                for (UiSubscription& subscription : ui_subscriptions) subscription = {};
                lease_open = false;
                lease_pending = false;
                return IoStatus::completed;
            }
            if (k_uptime_get() >= deadline) return IoStatus::timeout;
            native.wake();
            k_msleep(1);
        }
    }

    k_tid_t application_thread;
    std::array<char, zephyr_ble_central_max_logical_name_bytes + 1> logical_name{};
    std::size_t logical_name_size = 0;
    BleNativeState native;
    std::array<UiOperation, zephyr_ble_central_max_pending_operations> ui_operations{};
    std::array<UiSubscription, zephyr_ble_central_max_subscriptions> ui_subscriptions{};
    std::array<BleAddress, zephyr_ble_central_max_scan_results> discovered_peers{};
    std::uint16_t discovered_peer_count = 0;
    ScanPeerRecord scan_delivery_record{};
    std::uint32_t next_sequence = 1;
    bool lease_open = false;
    bool lease_pending = false;
    std::uint64_t lease_owner = 0;
    DeviceHandle lease_device{};
};

} // namespace mb::zephyr

namespace mb::zephyr {

ZephyrBleCentralBackend::ZephyrBleCentralBackend(const ZephyrBleCentralConfig& config)
    : m_state(std::make_unique<State>(config)) {}

ZephyrBleCentralBackend::~ZephyrBleCentralBackend() {
    if (!m_state) return;
    m_state->assert_application_thread();
    if (m_state->shutdown() != IoStatus::completed) std::terminate();
}

IoStatus ZephyrBleCentralBackend::shutdown() noexcept {
    if (!m_state) return IoStatus::completed;
    m_state->assert_application_thread();
    return m_state->shutdown();
}

std::span<const DeviceOperation> ZephyrBleCentralBackend::operations() const noexcept {
    m_state->assert_application_thread();
    return kImplementedOperations;
}

IoStatus ZephyrBleCentralBackend::start_operation(
    const BackendOperation& operation,
    std::weak_ptr<PeripheralCompletionSink> completions) {
    m_state->assert_application_thread();
    return m_state->start(operation, std::move(completions));
}

IoStatus ZephyrBleCentralBackend::cancel_operation(const BackendOperation& operation) {
    m_state->assert_application_thread();
    return m_state->cancel(operation);
}

void ZephyrBleCentralBackend::release_application(std::uint64_t owner_generation) {
    m_state->assert_application_thread();
    m_state->release(owner_generation);
}

void ZephyrBleCentralBackend::poll_on_application_thread() {
    m_state->assert_application_thread();
    m_state->poll();
}

std::shared_ptr<ZephyrBleCentralBackend> register_zephyr_ble_central(
    PeripheralRegistry& registry, const ZephyrBleCentralConfig& config) {
    std::shared_ptr<ZephyrBleCentralBackend> backend(new ZephyrBleCentralBackend(config));
    registry.register_endpoint(
        EndpointDescriptor{config.logical_name, zephyr_ble_central_version, false},
        config.access, backend,
        [](PeripheralBackend& value) {
            static_cast<ZephyrBleCentralBackend&>(value).poll_on_application_thread();
        });
    return backend;
}

} // namespace mb::zephyr
