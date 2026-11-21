#include "sdkconfig.h"
#if defined(CONFIG_MB_BLE_CENTRAL)
#include "esp_idf_ble_central.h"

#include "esp_idf_worker.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "os/os_mbuf.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mb {
namespace {

constexpr char kLogTag[] = "mb_blec";
constexpr std::size_t kWorkerQueueLength = 48;
constexpr std::uint32_t kWorkerStackBytes = 6144;
constexpr UBaseType_t kWorkerPriority = 5;
constexpr std::uint32_t kMaximumNativeDurationMs = 120'000;
constexpr std::uint32_t kShutdownIoTimeoutMs = kMaximumNativeDurationMs + 10'000;
constexpr std::uint16_t kNoConnection = BLE_HS_CONN_HANDLE_NONE;
constexpr std::size_t kNoSubscription = std::numeric_limits<std::size_t>::max();

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

[[nodiscard]] bool valid_uuid(const BleUuid& uuid, bool wildcard_allowed) noexcept {
    if (uuid.length == 0) return wildcard_allowed;
    if (uuid.length != 2 && uuid.length != 16) return false;
    return std::all_of(uuid.bytes.begin() + uuid.length, uuid.bytes.end(),
                       [](std::uint8_t byte) { return byte == 0; });
}

[[nodiscard]] bool valid_attribute(const BleAttribute& attribute,
                                   bool wildcard_allowed = false) noexcept {
    return valid_uuid(attribute.service, wildcard_allowed) &&
           valid_uuid(attribute.characteristic, wildcard_allowed);
}

[[nodiscard]] bool attribute_matches_filter(const BleAttribute& attribute,
                                            const BleAttribute& filter) noexcept {
    return (filter.service.length == 0 || same_uuid(filter.service, attribute.service)) &&
           (filter.characteristic.length == 0 ||
            same_uuid(filter.characteristic, attribute.characteristic));
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

[[nodiscard]] IoStatus att_error_status(int error) noexcept {
    switch (error) {
    case BLE_ATT_ERR_INVALID_HANDLE:
    case BLE_ATT_ERR_ATTR_NOT_FOUND:
    case BLE_ATT_ERR_INVALID_OFFSET:
    case BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN:
        return IoStatus::invalid_argument;
    case BLE_ATT_ERR_READ_NOT_PERMITTED:
    case BLE_ATT_ERR_WRITE_NOT_PERMITTED:
    case BLE_ATT_ERR_REQ_NOT_SUPPORTED:
    case BLE_ATT_ERR_ATTR_NOT_LONG:
    case BLE_ATT_ERR_UNSUPPORTED_GROUP:
        return IoStatus::unsupported;
    case BLE_ATT_ERR_INSUFFICIENT_AUTHEN:
    case BLE_ATT_ERR_INSUFFICIENT_AUTHOR:
    case BLE_ATT_ERR_INSUFFICIENT_KEY_SZ:
    case BLE_ATT_ERR_INSUFFICIENT_ENC:
        return IoStatus::permission_denied;
    case BLE_ATT_ERR_PREPARE_QUEUE_FULL:
    case BLE_ATT_ERR_INSUFFICIENT_RES:
        return IoStatus::memory_exhausted;
    default:
        return IoStatus::io_failure;
    }
}

[[nodiscard]] IoStatus hci_error_status(int error) noexcept {
    switch (error) {
    case BLE_ERR_PAGE_TMO:
    case BLE_ERR_CONN_SPVN_TMO:
    case BLE_ERR_CONN_ACCEPT_TMO:
    case BLE_ERR_LMP_LL_RSP_TMO:
        return IoStatus::timeout;
    case BLE_ERR_MEM_CAPACITY:
    case BLE_ERR_CONN_LIMIT:
    case BLE_ERR_CONN_REJ_RESOURCES:
        return IoStatus::memory_exhausted;
    case BLE_ERR_AUTH_FAIL:
    case BLE_ERR_PINKEY_MISSING:
    case BLE_ERR_CONN_REJ_SECURITY:
    case BLE_ERR_NO_PAIRING:
        return IoStatus::permission_denied;
    case BLE_ERR_UNSUPPORTED:
    case BLE_ERR_UNSUPP_REM_FEATURE:
    case BLE_ERR_NO_ROLE_CHANGE:
        return IoStatus::unsupported;
    case BLE_ERR_UNK_CONN_ID:
        return IoStatus::device_lost;
    case BLE_ERR_INV_HCI_CMD_PARMS:
    case BLE_ERR_CONN_REJ_BD_ADDR:
        return IoStatus::invalid_argument;
    case BLE_ERR_CMD_DISALLOWED:
    case BLE_ERR_REPEATED_ATTEMPTS:
        return IoStatus::busy;
    default:
        return IoStatus::io_failure;
    }
}

[[nodiscard]] IoStatus nimble_status(int error) noexcept {
    if (error >= BLE_HS_ERR_ATT_BASE && error < BLE_HS_ERR_HCI_BASE) {
        return att_error_status(error - BLE_HS_ERR_ATT_BASE);
    }
    if (error >= BLE_HS_ERR_HCI_BASE && error < BLE_HS_ERR_L2C_BASE) {
        return hci_error_status(error - BLE_HS_ERR_HCI_BASE);
    }
    switch (error) {
    case 0:
    case BLE_HS_EDONE:
        return IoStatus::completed;
    case BLE_HS_EAGAIN:
    case BLE_HS_EALREADY:
    case BLE_HS_EBUSY:
    case BLE_HS_EPREEMPTED:
    case BLE_HS_ESTALLED:
        return IoStatus::busy;
    case BLE_HS_EINVAL:
    case BLE_HS_EBADDATA:
    case BLE_HS_ENOENT:
    case BLE_HS_ENOADDR:
    case BLE_HS_EREJECT:
        return IoStatus::invalid_argument;
    case BLE_HS_ENOMEM:
    case BLE_HS_ENOMEM_EVT:
        return IoStatus::memory_exhausted;
    case BLE_HS_ENOTCONN:
    case BLE_HS_EDISABLED:
        return IoStatus::device_lost;
    case BLE_HS_ENOTSUP:
    case BLE_HS_EROLE:
        return IoStatus::unsupported;
    case BLE_HS_ETIMEOUT:
    case BLE_HS_ETIMEOUT_HCI:
        return IoStatus::timeout;
    case BLE_HS_EAUTHEN:
    case BLE_HS_EAUTHOR:
    case BLE_HS_EENCRYPT:
    case BLE_HS_EENCRYPT_KEY_SZ:
        return IoStatus::permission_denied;
    case BLE_HS_ESTORE_CAP:
        return IoStatus::memory_exhausted;
    default:
        return IoStatus::io_failure;
    }
}

[[noreturn]] void throw_esp_error(std::string_view action, esp_err_t error) {
    std::string message(action);
    message += ": ";
    message += esp_err_to_name(error);
    throw std::runtime_error(message);
}

void check_esp_call(esp_err_t error, std::string_view action) {
    if (error != ESP_OK) throw_esp_error(action, error);
}

[[nodiscard]] BleAddress portable_address(const ble_addr_t& address) noexcept {
    BleAddress result;
    std::copy(std::begin(address.val), std::end(address.val), result.bytes.begin());
    result.random = address.type == BLE_ADDR_RANDOM || address.type == BLE_ADDR_RANDOM_ID;
    return result;
}

[[nodiscard]] ble_addr_t native_address(const BleAddress& address) noexcept {
    ble_addr_t result{};
    result.type = address.random ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
    std::copy(address.bytes.begin(), address.bytes.end(), std::begin(result.val));
    return result;
}

[[nodiscard]] std::optional<BleUuid> portable_uuid(const ble_uuid_any_t& uuid) noexcept {
    BleUuid result;
    if (uuid.u.type == BLE_UUID_TYPE_16) {
        const std::uint16_t value = uuid.u16.value;
        result.length = 2;
        result.bytes[0] = static_cast<std::uint8_t>(value);
        result.bytes[1] = static_cast<std::uint8_t>(value >> 8U);
        return result;
    }
    if (uuid.u.type == BLE_UUID_TYPE_128) {
        result.length = 16;
        std::copy(std::begin(uuid.u128.value), std::end(uuid.u128.value),
                  result.bytes.begin());
        return result;
    }
    return std::nullopt;
}

std::atomic<void*> g_active_state{nullptr};

} // namespace

struct EspIdfBleCentralBackend::State final {
    struct EndpointState;
    struct ServiceRecord {
        BleUuid uuid{};
        std::uint16_t start_handle = 0;
        std::uint16_t end_handle = 0;
    };

    struct CharacteristicRecord {
        BleAttribute attribute{};
        std::uint16_t definition_handle = 0;
        std::uint16_t value_handle = 0;
        std::uint8_t properties = 0;
        std::uint16_t cccd_handle = 0;
        bool cccd_discovered = false;
    };

    enum class WorkState : std::uint8_t { free, submitted, queued, native };
    enum class WorkPhase : std::uint8_t {
        none,
        scan,
        connect,
        services,
        characteristics,
        descriptors,
        read,
        write,
        subscribe_cccd,
        subscribe_rollback,
        unsubscribe_cccd,
        disconnect,
    };

    struct WorkRecord {
        BackendOperation operation{};
        std::weak_ptr<PeripheralCompletionSink> completions;
        struct EndpointState* endpoint = nullptr;
        WorkState state = WorkState::free;
        std::uint32_t submission_token = 0;
        WorkPhase phase = WorkPhase::none;
        std::atomic<bool> cancellation_requested{false};
        std::atomic<bool> retirement_requested{false};
        std::uint32_t scan_token = 0;
        bool native_abort_sent = false;
        bool scan_stopping = false;
        IoStatus partial_status = IoStatus::completed;
        std::uint32_t service_index = 0;
        std::size_t characteristic_index = 0;
        std::size_t subscription_index = kNoSubscription;
        std::vector<BlePeer> scan_peers;
        ByteBuffer read_value;
    };

    struct SubscriptionRecord {
        bool reserved = false;
        bool active = false;
        std::atomic<bool> retiring{false};
        BleSubscriptionTarget target = BleSubscriptionTarget::connection_state;
        BleAttribute attribute{};
        std::uint16_t value_handle = 0;
        std::uint16_t cccd_handle = 0;
        std::uint32_t maximum_bytes_per_buffer = 0;
        std::uint64_t owner_generation = 0;
        DeviceHandle subscription{};
        std::weak_ptr<PeripheralCompletionSink> completions;
    };

    struct EndpointState {
        State* owner = nullptr;
        EspBleCentralEndpointConfig config{};
        std::array<char, esp_idf_ble_central_max_logical_name_bytes> logical_name{};
        std::size_t logical_name_size = 0;

        bool open = false;
        std::uint64_t owner_generation = 0;
        DeviceHandle device{};
        std::atomic<bool> close_requested{false};
        WorkRecord* close_work = nullptr;

        bool connected = false;
        bool paired = false;
        bool disconnect_requested = false;
        bool disconnect_local_pending = false;
        BleAddress peer{};
        std::uint16_t connection_handle = kNoConnection;
        WorkRecord* disconnect_work = nullptr;
        WorkRecord* gatt_work = nullptr;
        std::atomic<WorkRecord*> scan_work{nullptr};
        std::atomic<std::uint32_t> scan_report_state{0};
        std::atomic<WorkRecord*> connect_work{nullptr};

        std::array<ServiceRecord, esp_idf_ble_central_max_services> services{};
        std::size_t service_count = 0;
        std::array<CharacteristicRecord, esp_idf_ble_central_max_characteristics> characteristics{};
        std::size_t characteristic_count = 0;
        bool attributes_complete = false;
        std::vector<BleAddress> discovered_peers;
        std::array<SubscriptionRecord, esp_idf_ble_central_max_subscriptions_per_endpoint>
            subscriptions{};
    };

    enum class MessageType : std::uint8_t {
        operation,
        advertisement,
        scan_complete,
        connect,
        disconnect,
        termination_failure,
        encryption_change,
        service,
        characteristic,
        descriptor,
        attribute,
        notification,
        host_sync,
        host_reset,
        wake,
        stop,
    };

    struct Message {
        MessageType type = MessageType::wake;
        EndpointState* endpoint = nullptr;
        WorkRecord* work = nullptr;
        std::uint16_t connection_handle = kNoConnection;
        std::uint16_t handle = 0;
        std::uint16_t end_handle = 0;
        std::uint16_t characteristic_value_handle = 0;
        std::uint16_t offset = 0;
        int status = 0;
        std::uint32_t length = 0;
        std::uint32_t submission_token = 0;
        std::uint32_t scan_token = 0;
        std::array<std::uint8_t, 6> address{};
        std::uint8_t address_type = 0;
        std::int8_t rssi = 0;
        std::uint8_t properties = 0;
        bool paired = false;
        bool copy_failed = false;
        ble_uuid_any_t uuid{};
        std::array<std::uint8_t, esp_idf_ble_central_max_attribute_bytes> bytes{};
    };
    static_assert(std::is_trivially_copyable_v<Message>);

    explicit State(std::span<const EspBleCentralEndpointConfig> configs) {
        validate_configs(configs);
        m_endpoint_count = configs.size();
        for (std::size_t index = 0; index < configs.size(); ++index) {
            EndpointState& endpoint = m_endpoints[index];
            endpoint.owner = this;
            endpoint.config = configs[index];
            endpoint.logical_name_size = configs[index].logical_name.size();
            std::copy(configs[index].logical_name.begin(), configs[index].logical_name.end(),
                      endpoint.logical_name.begin());
        }
        try {
            create_native_objects();
            claim_process_stack();
            initialize_nimble();
            start_worker();
            nimble_port_freertos_init(&State::host_task_entry);
            m_host_task_started = true;
        } catch (...) {
            cleanup_failed_construction();
            throw;
        }
    }

    ~State() = default;

    static void validate_configs(std::span<const EspBleCentralEndpointConfig> configs) {
        if (configs.size() > esp_idf_ble_central_max_endpoints) {
            throw std::invalid_argument("ESP BLE central supports at most four endpoints");
        }
        for (std::size_t index = 0; index < configs.size(); ++index) {
            const auto& config = configs[index];
            if (config.logical_name.empty() ||
                config.logical_name.size() > esp_idf_ble_central_max_logical_name_bytes) {
                throw std::invalid_argument("ESP BLE central logical names must contain 1..63 bytes");
            }
            if (config.scan_duration_ms == 0 || config.connect_timeout_ms == 0 ||
                config.scan_duration_ms > kMaximumNativeDurationMs ||
                config.connect_timeout_ms > kMaximumNativeDurationMs) {
                throw std::invalid_argument("ESP BLE central durations must be 1..120000 ms");
            }
            for (std::size_t previous = 0; previous < index; ++previous) {
                if (configs[previous].logical_name == config.logical_name) {
                    throw std::invalid_argument("duplicate ESP BLE central logical endpoint name");
                }
            }
        }
    }

    [[nodiscard]] std::string_view logical_name(const EndpointState& endpoint) const noexcept {
        return {endpoint.logical_name.data(), endpoint.logical_name_size};
    }

    [[nodiscard]] EndpointState* find_endpoint(std::string_view name) noexcept {
        for (std::size_t index = 0; index < m_endpoint_count; ++index) {
            if (logical_name(m_endpoints[index]) == name) return &m_endpoints[index];
        }
        return nullptr;
    }

    void create_native_objects() {
        m_worker_queue = xQueueCreateStatic(kWorkerQueueLength, sizeof(Message),
                                            m_queue_storage.data(), &m_queue_control);
        m_worker_done = xSemaphoreCreateBinaryStatic(&m_worker_done_control);
        m_host_done = xSemaphoreCreateBinaryStatic(&m_host_done_control);
        if (m_worker_queue == nullptr || m_worker_done == nullptr || m_host_done == nullptr) {
            throw std::runtime_error("creating static ESP BLE central FreeRTOS objects failed");
        }
    }

    void delete_native_objects() noexcept {
        if (m_worker_queue != nullptr) {
            vQueueDelete(m_worker_queue);
            m_worker_queue = nullptr;
        }
        if (m_worker_done != nullptr) {
            vSemaphoreDelete(m_worker_done);
            m_worker_done = nullptr;
        }
        if (m_host_done != nullptr) {
            vSemaphoreDelete(m_host_done);
            m_host_done = nullptr;
        }
    }

    void claim_process_stack() {
        void* expected = nullptr;
        if (!g_active_state.compare_exchange_strong(expected, this, std::memory_order_acq_rel)) {
            throw std::runtime_error("another ESP BLE central backend owns NimBLE");
        }
        m_process_stack_claimed = true;
    }

    void release_process_stack() noexcept {
        if (!m_process_stack_claimed) return;
        if (m_host_callbacks_installed) {
            ble_hs_cfg.sync_cb = nullptr;
            ble_hs_cfg.reset_cb = nullptr;
            m_host_callbacks_installed = false;
        }
        void* expected = this;
        (void)g_active_state.compare_exchange_strong(expected, nullptr,
                                                    std::memory_order_acq_rel);
        m_process_stack_claimed = false;
    }

    void initialize_nimble() {
        check_esp_call(nvs_flash_init(), "initializing NVS for NimBLE");
        check_esp_call(nimble_port_init(), "initializing NimBLE controller and host");
        m_port_initialized = true;
        ble_hs_cfg.reset_cb = &State::host_reset_entry;
        ble_hs_cfg.sync_cb = &State::host_sync_entry;
        m_host_callbacks_installed = true;
        ble_hs_cfg.sm_bonding = 0;
        ble_hs_cfg.sm_mitm = 0;
        ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
        ble_hs_cfg.sm_our_key_dist = 0;
        ble_hs_cfg.sm_their_key_dist = 0;
    }

    void start_worker() {
        m_worker = xTaskCreateStatic(
            &State::worker_entry, "mb_blec", kWorkerStackBytes, this, kWorkerPriority,
            reinterpret_cast<StackType_t*>(m_worker_stack.data()), &m_worker_control);
        if (m_worker == nullptr) {
            throw std::runtime_error("creating static ESP BLE central worker task failed");
        }
        m_worker_started = true;
    }

    void cleanup_failed_construction() noexcept {
        if (m_worker_started) {
            Message message{};
            message.type = MessageType::stop;
            (void)xQueueSend(m_worker_queue, &message, detail::worker_shutdown_ticks(0));
            (void)xSemaphoreTake(m_worker_done, detail::worker_shutdown_ticks(0));
            m_worker_started = false;
        }
        if (m_port_initialized) {
            const esp_err_t status = nimble_port_deinit();
            if (status != ESP_OK) {
                ESP_LOGE(kLogTag, "deinitializing failed NimBLE construction: %s",
                         esp_err_to_name(status));
            }
            m_port_initialized = false;
        }
        release_process_stack();
        delete_native_objects();
    }

    void send_persistent(Message& message) noexcept {
        // NimBLE invokes callbacks on its host task. Queue storage outlives the
        // host task, and waiting here prevents a completed ATT procedure from
        // being reported as a partial result when the bounded queue is full.
        if (!m_callbacks_accepting.load(std::memory_order_acquire)) {
            (void)xQueueSend(m_worker_queue, &message, 0);
            return;
        }
        (void)xQueueSend(m_worker_queue, &message, portMAX_DELAY);
    }

    [[nodiscard]] bool send_best_effort(Message& message) noexcept {
        return xQueueSend(m_worker_queue, &message, 0) == pdTRUE;
    }

    static State* active_state() noexcept {
        return static_cast<State*>(g_active_state.load(std::memory_order_acquire));
    }

    static void host_sync_entry() {
        State* state = active_state();
        if (state == nullptr) return;
        Message message{};
        message.type = MessageType::host_sync;
        message.status = ble_hs_util_ensure_addr(0);
        state->send_persistent(message);
    }

    static void host_reset_entry(int reason) {
        State* state = active_state();
        if (state == nullptr) return;
        Message message{};
        message.type = MessageType::host_reset;
        message.status = reason;
        state->send_persistent(message);
    }

    static void host_task_entry(void*) {
        State* state = active_state();
        if (state == nullptr) {
            nimble_port_freertos_deinit();
            return;
        }
        nimble_port_run();
        xSemaphoreGive(state->m_host_done);
        nimble_port_freertos_deinit();
    }

    static int copy_mbuf(const os_mbuf* mbuf, Message& message) noexcept {
        if (mbuf == nullptr) {
            message.length = 0;
            return 0;
        }
        const int packet_length = OS_MBUF_PKTLEN(mbuf);
        if (packet_length < 0) {
            message.copy_failed = true;
            return BLE_HS_EBADDATA;
        }
        message.length = static_cast<std::uint32_t>(packet_length);
        if (message.length > message.bytes.size()) {
            message.copy_failed = true;
            return 0;
        }
        if (message.length != 0 &&
            os_mbuf_copydata(const_cast<os_mbuf*>(mbuf), 0,
                             static_cast<int>(message.length), message.bytes.data()) != 0) {
            message.copy_failed = true;
        }
        return 0;
    }
    static void post_advertisement(EndpointState& endpoint, Message& message,
                                   const ble_addr_t& address, std::int8_t rssi,
                                   const std::uint8_t* data, std::size_t length) {
        const std::uint32_t report_state =
            endpoint.scan_report_state.load(std::memory_order_acquire);
        message.scan_token = report_state & ~std::uint32_t{1};
        message.address_type = address.type;
        std::copy(std::begin(address.val), std::end(address.val), message.address.begin());
        message.rssi = rssi;
        message.length = length;
        if ((length != 0 && data == nullptr) ||
            length > esp_idf_ble_central_max_advertisement_bytes) {
            message.copy_failed = true;
        } else if (length != 0) {
            std::copy_n(data, length, message.bytes.begin());
        }
        if (!endpoint.owner->send_best_effort(message)) {
            // Compare-and-swap keeps a late failed report from changing the
            // loss state of a newer scan generation.
            std::uint32_t expected = message.scan_token;
            (void)endpoint.scan_report_state.compare_exchange_strong(
                expected, message.scan_token | 1U, std::memory_order_acq_rel);
        }
    }


    static int gap_event(ble_gap_event* event, void* argument) {
        auto& endpoint = *static_cast<EndpointState*>(argument);
        State& state = *endpoint.owner;
        Message message{};
        message.endpoint = &endpoint;

        switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            message.type = MessageType::advertisement;
            const auto& report = event->disc;
            post_advertisement(endpoint, message, report.addr, report.rssi,
                               report.data, report.length_data);
            return 0;
        }
#if MYNEWT_VAL(BLE_EXT_ADV)
        case BLE_GAP_EVENT_EXT_DISC: {
            message.type = MessageType::advertisement;
            const auto& report = event->ext_disc;
            message.copy_failed = report.data_status != BLE_GAP_EXT_ADV_DATA_STATUS_COMPLETE;
            post_advertisement(endpoint, message, report.addr, report.rssi,
                               report.data, report.length_data);
            return 0;
        }
#endif
        case BLE_GAP_EVENT_DISC_COMPLETE:
            message.type = MessageType::scan_complete;
            message.status = event->disc_complete.reason;
            state.send_persistent(message);
            return 0;
        case BLE_GAP_EVENT_CONNECT:
            message.type = MessageType::connect;
            message.status = event->connect.status;
            message.connection_handle = event->connect.conn_handle;
            if (message.status == 0) {
                ble_gap_conn_desc description{};
                if (ble_gap_conn_find(message.connection_handle, &description) == 0) {
                    const BleAddress peer = portable_address(description.peer_id_addr);
                    message.address = peer.bytes;
                    message.address_type = description.peer_id_addr.type;
                    message.paired = description.sec_state.bonded != 0;
                } else {
                    message.copy_failed = true;
                }
            }
            state.send_persistent(message);
            return 0;
        case BLE_GAP_EVENT_TERM_FAILURE:
            message.type = MessageType::termination_failure;
            message.status = event->term_failure.status;
            message.connection_handle = event->term_failure.conn_handle;
            state.send_persistent(message);
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            message.type = MessageType::disconnect;
            message.status = event->disconnect.reason;
            message.connection_handle = event->disconnect.conn.conn_handle;
            message.paired = event->disconnect.conn.sec_state.bonded != 0;
            message.address_type = event->disconnect.conn.peer_id_addr.type;
            std::copy(event->disconnect.conn.peer_id_addr.val,
                      event->disconnect.conn.peer_id_addr.val + message.address.size(),
                      message.address.begin());
            state.send_persistent(message);
            return 0;
        case BLE_GAP_EVENT_NOTIFY_RX:
            message.type = MessageType::notification;
            message.connection_handle = event->notify_rx.conn_handle;
            message.handle = event->notify_rx.attr_handle;
            (void)copy_mbuf(event->notify_rx.om, message);
            static_cast<void>(state.send_best_effort(message));
            return 0;
        case BLE_GAP_EVENT_ENC_CHANGE:
            message.type = MessageType::encryption_change;
            message.status = event->enc_change.status;
            message.connection_handle = event->enc_change.conn_handle;
            if (message.status == 0) {
                ble_gap_conn_desc description{};
                if (ble_gap_conn_find(message.connection_handle, &description) == 0) {
                    message.paired = description.sec_state.bonded != 0;
                }
            }
            state.send_persistent(message);
            return 0;
        case BLE_GAP_EVENT_PASSKEY_ACTION:
            return BLE_HS_ENOTSUP;
        case BLE_GAP_EVENT_REPEAT_PAIRING:
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        default:
            return 0;
        }
    }

    template <typename Callback>
    static int copy_discovery_callback(WorkRecord* work, std::uint16_t connection_handle,
                                       MessageType type, const ble_gatt_error* error,
                                       Callback copy_body) {
        auto& endpoint = *work->endpoint;
        Message message{};
        message.type = type;
        message.endpoint = &endpoint;
        message.work = work;
        message.connection_handle = connection_handle;
        message.status = error->status;
        copy_body(message);
        endpoint.owner->send_persistent(message);
        return 0;
    }

    static int service_discovered(std::uint16_t connection_handle,
                                  const ble_gatt_error* error,
                                  const ble_gatt_svc* service, void* argument) {
        return copy_discovery_callback(
            static_cast<WorkRecord*>(argument), connection_handle, MessageType::service, error,
            [&](Message& message) {
                if (service != nullptr) {
                    message.handle = service->start_handle;
                    message.end_handle = service->end_handle;
                    message.uuid = service->uuid;
                }
            });
    }

    static int characteristic_discovered(std::uint16_t connection_handle,
                                         const ble_gatt_error* error,
                                         const ble_gatt_chr* characteristic, void* argument) {
        return copy_discovery_callback(
            static_cast<WorkRecord*>(argument), connection_handle, MessageType::characteristic,
            error, [&](Message& message) {
                if (characteristic != nullptr) {
                    message.handle = characteristic->def_handle;
                    message.characteristic_value_handle = characteristic->val_handle;
                    message.properties = characteristic->properties;
                    message.uuid = characteristic->uuid;
                }
            });
    }

    static int descriptor_discovered(std::uint16_t connection_handle,
                                     const ble_gatt_error* error,
                                     std::uint16_t characteristic_value_handle,
                                     const ble_gatt_dsc* descriptor, void* argument) {
        return copy_discovery_callback(
            static_cast<WorkRecord*>(argument), connection_handle, MessageType::descriptor,
            error, [&](Message& message) {
                message.characteristic_value_handle = characteristic_value_handle;
                if (descriptor != nullptr) {
                    message.handle = descriptor->handle;
                    message.uuid = descriptor->uuid;
                }
            });
    }

    static int attribute_completed(std::uint16_t connection_handle,
                                   const ble_gatt_error* error,
                                   ble_gatt_attr* attribute, void* argument) {
        auto& work = *static_cast<WorkRecord*>(argument);
        Message message{};
        message.type = MessageType::attribute;
        message.endpoint = work.endpoint;
        message.work = &work;
        message.connection_handle = connection_handle;
        message.status = error->status;
        if (attribute != nullptr) {
            message.handle = attribute->handle;
            message.offset = attribute->offset;
            (void)copy_mbuf(attribute->om, message);
        } else if (error->status == 0 && work.phase == WorkPhase::read) {
            message.copy_failed = true;
        }
        work.endpoint->owner->send_persistent(message);
        return 0;
    }

    static void worker_entry(void* argument) {
        auto& state = *static_cast<State*>(argument);
        state.run_worker();
        xSemaphoreGive(state.m_worker_done);
        vTaskDelete(nullptr);
    }

    [[nodiscard]] IoStatus start(
        const BackendOperation& operation,
        std::weak_ptr<PeripheralCompletionSink> completions) noexcept {
        EndpointState* const endpoint = find_endpoint(operation.logical_name);
        if (endpoint == nullptr || operation.owner_generation == 0 ||
            operation.operation.is_empty() || !request_matches(operation)) {
            return IoStatus::invalid_argument;
        }
        const IoStatus request_status = validate_request(operation);
        if (request_status != IoStatus::completed) return request_status;

        WorkRecord* selected = nullptr;
        bool accepting = false;
        portENTER_CRITICAL(&m_mutex);
        accepting = m_accepting;
        if (accepting) {
            for (WorkRecord& work : m_work) {
                if (work.state != WorkState::free) continue;
                selected = &work;
                work.operation = operation;
                work.completions = std::move(completions);
                work.endpoint = endpoint;
                ++m_next_submission_token;
                if (m_next_submission_token == 0) ++m_next_submission_token;
                work.submission_token = m_next_submission_token;
                work.state = WorkState::submitted;
                work.phase = WorkPhase::none;
                work.cancellation_requested.store(false, std::memory_order_release);
                work.retirement_requested.store(false, std::memory_order_release);
                work.native_abort_sent = false;
                work.scan_token = 0;
                work.scan_stopping = false;
                work.partial_status = IoStatus::completed;
                work.service_index = 0;
                work.characteristic_index = 0;
                work.subscription_index = kNoSubscription;
                work.scan_peers.clear();
                work.read_value.clear();
                break;
            }
        }
        portEXIT_CRITICAL(&m_mutex);
        if (selected == nullptr) return accepting ? IoStatus::busy : IoStatus::device_lost;

        Message message{};
        message.type = MessageType::operation;
        message.work = selected;
        message.submission_token = selected->submission_token;
        if (xQueueSend(m_worker_queue, &message, 0) != pdTRUE) {
            portENTER_CRITICAL(&m_mutex);
            if (selected->state == WorkState::submitted &&
                selected->submission_token == message.submission_token) {
                reset_work(*selected);
            }
            portEXIT_CRITICAL(&m_mutex);
            return IoStatus::busy;
        }
        return IoStatus::completed;
    }

    [[nodiscard]] IoStatus validate_request(const BackendOperation& operation) const noexcept {
        if (operation.budget.maximum_bytes_per_buffer == 0) {
            return IoStatus::invalid_argument;
        }
        if (operation.id == DeviceOperation::open) {
            const CapabilityVersion minimum =
                std::get<OpenRequest>(operation.request).minimum_version;
            if (minimum.capability != Capability::ble_central) {
                return IoStatus::invalid_argument;
            }
            if (minimum.major != peripheral_abi_major || minimum.minor != 0) {
                return IoStatus::unsupported;
            }
            return IoStatus::completed;
        }
        if (operation.device.capability != Capability::ble_central ||
            operation.device.owner_generation != operation.owner_generation) {
            return IoStatus::invalid_argument;
        }
        if (const auto* discover = std::get_if<BleDiscoverRequest>(&operation.request)) {
            if (operation.id != DeviceOperation::discover || discover->maximum_results == 0) {
                return IoStatus::invalid_argument;
            }
            switch (discover->kind) {
            case BleDiscoveryKind::scan:
                return discover->filter.service.length == 0 &&
                               discover->filter.characteristic.length == 0
                           ? IoStatus::completed
                           : IoStatus::invalid_argument;
            case BleDiscoveryKind::services:
            case BleDiscoveryKind::characteristics:
                return valid_attribute(discover->filter, true)
                           ? IoStatus::completed
                           : IoStatus::invalid_argument;
            default:
                return IoStatus::invalid_argument;
            }
        }
        if (const auto* connect = std::get_if<BleConnectRequest>(&operation.request)) {
            constexpr std::uint32_t minimum_supervision_ms = 100;
            constexpr std::uint32_t maximum_supervision_ms = 32'000;
            if (operation.id != DeviceOperation::connect ||
                connect->supervision_timeout_ms < minimum_supervision_ms ||
                connect->supervision_timeout_ms > maximum_supervision_ms ||
                connect->supervision_timeout_ms % 10U != 0) {
                return IoStatus::invalid_argument;
            }
            return IoStatus::completed;
        }
        if (const auto* read = std::get_if<BleReadRequest>(&operation.request)) {
            return operation.id == DeviceOperation::read && valid_attribute(read->attribute)
                       ? IoStatus::completed
                       : IoStatus::invalid_argument;
        }
        if (const auto* write = std::get_if<BleWriteRequest>(&operation.request)) {
            if (operation.id != DeviceOperation::write || !valid_attribute(write->attribute) ||
                write->value == nullptr || write->value->empty() ||
                (write->mode != BleWriteMode::with_response &&
                 write->mode != BleWriteMode::without_response)) {
                return IoStatus::invalid_argument;
            }
            const std::size_t limit = std::min<std::size_t>(
                esp_idf_ble_central_max_attribute_bytes,
                operation.budget.maximum_bytes_per_buffer);
            return write->value->size() <= limit ? IoStatus::completed
                                                  : IoStatus::memory_exhausted;
        }
        if (const auto* subscribe = std::get_if<BleSubscribeRequest>(&operation.request)) {
            if (operation.id != DeviceOperation::subscribe ||
                operation.subscription.is_empty() ||
                operation.subscription.owner_generation != operation.owner_generation) {
                return IoStatus::invalid_argument;
            }
            switch (subscribe->target) {
            case BleSubscriptionTarget::connection_state:
                return subscribe->attribute.service.length == 0 &&
                               subscribe->attribute.characteristic.length == 0
                           ? IoStatus::completed
                           : IoStatus::invalid_argument;
            case BleSubscriptionTarget::characteristic:
                return valid_attribute(subscribe->attribute) ? IoStatus::completed
                                                              : IoStatus::invalid_argument;
            default:
                return IoStatus::invalid_argument;
            }
        }
        if (operation.id == DeviceOperation::disconnect) {
            return std::holds_alternative<BleDisconnectRequest>(operation.request)
                       ? IoStatus::completed
                       : IoStatus::invalid_argument;
        }
        if (operation.id == DeviceOperation::unsubscribe) {
            return std::get<UnsubscribeRequest>(operation.request).subscription.is_empty()
                       ? IoStatus::invalid_argument
                       : IoStatus::completed;
        }
        return IoStatus::completed;
    }

    [[nodiscard]] IoStatus cancel(const BackendOperation& operation) noexcept {
        IoStatus result = IoStatus::invalid_argument;
        portENTER_CRITICAL(&m_mutex);
        for (WorkRecord& work : m_work) {
            if (work.state == WorkState::free ||
                work.operation.owner_generation != operation.owner_generation ||
                work.operation.operation != operation.operation) {
                continue;
            }
            work.cancellation_requested.store(true, std::memory_order_release);
            result = IoStatus::completed;
            break;
        }
        portEXIT_CRITICAL(&m_mutex);
        if (result == IoStatus::completed) wake_worker();
        return result;
    }

    void release(std::uint64_t owner_generation) noexcept {
        portENTER_CRITICAL(&m_mutex);
        for (WorkRecord& work : m_work) {
            if (work.state != WorkState::free &&
                work.operation.owner_generation == owner_generation) {
                work.retirement_requested.store(true, std::memory_order_release);
            }
        }
        for (std::size_t index = 0; index < m_endpoint_count; ++index) {
            EndpointState& endpoint = m_endpoints[index];
            if (!endpoint.open || endpoint.owner_generation != owner_generation) continue;
            endpoint.close_requested.store(true, std::memory_order_release);
            for (SubscriptionRecord& subscription : endpoint.subscriptions) {
                if (subscription.reserved && subscription.owner_generation == owner_generation) {
                    subscription.retiring.store(true, std::memory_order_release);
                }
            }
        }
        portEXIT_CRITICAL(&m_mutex);
        wake_worker();
    }

    void wake_worker() noexcept {
        Message message{};
        (void)send_best_effort(message);
    }

    [[nodiscard]] bool cancelled(const WorkRecord& work) const noexcept {
        return work.cancellation_requested.load(std::memory_order_acquire) ||
               work.retirement_requested.load(std::memory_order_acquire);
    }

    void reset_work(WorkRecord& work) noexcept {
        if (work.endpoint != nullptr) {
            EndpointState& endpoint = *work.endpoint;
            if (endpoint.gatt_work == &work) endpoint.gatt_work = nullptr;
            if (endpoint.disconnect_work == &work) endpoint.disconnect_work = nullptr;
            if (endpoint.close_work == &work) endpoint.close_work = nullptr;
            if (endpoint.scan_work.load(std::memory_order_relaxed) == &work) {
                endpoint.scan_work.store(nullptr, std::memory_order_release);
            }
            if (endpoint.connect_work.load(std::memory_order_relaxed) == &work) {
                endpoint.connect_work.store(nullptr, std::memory_order_release);
            }
            if (m_scan_work == &work) m_scan_work = nullptr;
            if (m_connect_work == &work) m_connect_work = nullptr;
            if (work.subscription_index != kNoSubscription) {
                SubscriptionRecord& subscription =
                    endpoint.subscriptions[work.subscription_index];
                if (!subscription.active) clear_subscription(subscription);
            }
        }
        work.scan_peers.clear();
        work.read_value.clear();
        work.operation = BackendOperation{};
        work.completions.reset();
        work.endpoint = nullptr;
        work.phase = WorkPhase::none;
        work.partial_status = IoStatus::completed;
        work.state = WorkState::free;
    }

    void finish_work(WorkRecord& work, IoStatus status,
                     DeviceResponse response) {
        if ((operation_is_native(work) || work.phase != WorkPhase::none) &&
            status != IoStatus::completed && cancelled(work)) {
            status = IoStatus::cancelled;
        }
        EndpointState* const endpoint = work.endpoint;
        const DeviceOperation id = work.operation.id;
        const auto completions = work.completions.lock();
        const bool delivered = completions != nullptr;
        if (completions) {
            completions->complete(work.operation, status, std::move(response));
        }
        portENTER_CRITICAL(&m_mutex);
        reset_work(work);
        portEXIT_CRITICAL(&m_mutex);
        if (!delivered && endpoint != nullptr && status == IoStatus::completed &&
            (id == DeviceOperation::open || id == DeviceOperation::connect ||
             id == DeviceOperation::subscribe)) {
            request_endpoint_close(*endpoint);
        }
    }

    [[nodiscard]] static bool operation_is_native(const WorkRecord& work) noexcept {
        switch (work.operation.id) {
        case DeviceOperation::discover:
        case DeviceOperation::connect:
        case DeviceOperation::disconnect:
        case DeviceOperation::read:
        case DeviceOperation::write:
        case DeviceOperation::subscribe:
        case DeviceOperation::unsubscribe:
        case DeviceOperation::close:
            return true;
        default:
            return false;
        }
    }

    void clear_subscription(SubscriptionRecord& subscription) noexcept {
        subscription.reserved = false;
        subscription.active = false;
        subscription.retiring.store(false, std::memory_order_release);
        subscription.attribute = BleAttribute{};
        subscription.value_handle = 0;
        subscription.cccd_handle = 0;
        subscription.maximum_bytes_per_buffer = 0;
        subscription.owner_generation = 0;
        subscription.subscription = DeviceHandle{};
        subscription.completions.reset();
    }

    void clear_characteristic_subscriptions(EndpointState& endpoint) noexcept {
        for (SubscriptionRecord& subscription : endpoint.subscriptions) {
            if (subscription.reserved &&
                subscription.target == BleSubscriptionTarget::characteristic) {
                clear_subscription(subscription);
            }
        }
    }

    void clear_attributes(EndpointState& endpoint) noexcept {
        endpoint.service_count = 0;
        endpoint.characteristic_count = 0;
        endpoint.attributes_complete = false;
        for (CharacteristicRecord& characteristic : endpoint.characteristics) {
            characteristic = CharacteristicRecord{};
        }
    }

    [[nodiscard]] IoStatus bind_lease(EndpointState& endpoint,
                                      const BackendOperation& operation) noexcept {
        portENTER_CRITICAL(&m_mutex);
        IoStatus status = IoStatus::completed;
        if (!endpoint.open || endpoint.owner_generation != operation.owner_generation) {
            status = IoStatus::device_lost;
        } else if (endpoint.close_requested.load(std::memory_order_acquire) &&
                   operation.id != DeviceOperation::close) {
            status = IoStatus::device_lost;
        } else if (endpoint.device.is_empty()) {
            endpoint.device = operation.device;
        } else if (endpoint.device != operation.device) {
            status = IoStatus::invalid_argument;
        }
        portEXIT_CRITICAL(&m_mutex);
        return status;
    }

    [[nodiscard]] bool native_ready() const noexcept { return m_synchronized; }

    void request_endpoint_close(EndpointState& endpoint) noexcept {
        endpoint.close_requested.store(true, std::memory_order_release);
        for (WorkRecord& work : m_work) {
            if (work.state != WorkState::free && work.endpoint == &endpoint &&
                &work != endpoint.close_work) {
                work.retirement_requested.store(true, std::memory_order_release);
            }
        }
        for (SubscriptionRecord& subscription : endpoint.subscriptions) {
            if (subscription.reserved) {
                subscription.retiring.store(true, std::memory_order_release);
            }
        }
    }

    void begin_close(WorkRecord& work) {
        EndpointState& endpoint = *work.endpoint;
        const IoStatus lease_status = bind_lease(endpoint, work.operation);
        if (lease_status != IoStatus::completed) {
            finish_work(work, lease_status, EmptyResponse{});
            return;
        }
        if (endpoint.close_work != nullptr && endpoint.close_work != &work) {
            finish_work(work, IoStatus::busy, EmptyResponse{});
            return;
        }
        endpoint.close_work = &work;
        request_endpoint_close(endpoint);
        stop_endpoint_activity(endpoint);
        if (close_can_finish(endpoint)) {
            finish_endpoint_close(endpoint);
        }
    }

    [[nodiscard]] bool endpoint_has_other_work(const EndpointState& endpoint,
                                               const WorkRecord* exception) const noexcept {
        for (const WorkRecord& work : m_work) {
            if (work.state != WorkState::free && work.endpoint == &endpoint &&
                &work != exception) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool close_can_finish(const EndpointState& endpoint) const noexcept {
        return endpoint.close_requested.load(std::memory_order_acquire) &&
               endpoint.close_work != nullptr && !endpoint.connected &&
               endpoint.disconnect_work == nullptr && endpoint.gatt_work == nullptr &&
               !endpoint.disconnect_local_pending &&
               endpoint.scan_work.load(std::memory_order_acquire) == nullptr &&
               endpoint.connect_work.load(std::memory_order_acquire) == nullptr &&
               !endpoint_has_other_work(endpoint, endpoint.close_work);
    }

    [[nodiscard]] bool release_can_finish(const EndpointState& endpoint) const noexcept {
        return endpoint.close_requested.load(std::memory_order_acquire) &&
               endpoint.close_work == nullptr && endpoint.open && !endpoint.connected &&
               endpoint.disconnect_work == nullptr && endpoint.gatt_work == nullptr &&
               !endpoint.disconnect_local_pending &&
               endpoint.scan_work.load(std::memory_order_acquire) == nullptr &&
               endpoint.connect_work.load(std::memory_order_acquire) == nullptr &&
               !endpoint_has_other_work(endpoint, nullptr);
    }

    [[nodiscard]] bool native_quiescent() const noexcept {
        if (m_scan_work != nullptr || m_connect_work != nullptr) return false;
        for (const WorkRecord& work : m_work) {
            if (work.state != WorkState::free) return false;
        }
        for (std::size_t index = 0; index < m_endpoint_count; ++index) {
            const EndpointState& endpoint = m_endpoints[index];
            if (endpoint.connected || endpoint.disconnect_work != nullptr ||
                endpoint.disconnect_local_pending || endpoint.gatt_work != nullptr ||
                endpoint.scan_work.load(std::memory_order_acquire) != nullptr ||
                endpoint.connect_work.load(std::memory_order_acquire) != nullptr) {
                return false;
            }
        }
        return true;
    }

    void clear_physical_connection(EndpointState& endpoint) noexcept {
        endpoint.connected = false;
        endpoint.disconnect_requested = false;
        endpoint.disconnect_local_pending = false;
        endpoint.connection_handle = kNoConnection;
        clear_attributes(endpoint);
        clear_characteristic_subscriptions(endpoint);
    }

    void finish_endpoint_close(EndpointState& endpoint) {
        WorkRecord* const close_work = endpoint.close_work;
        for (SubscriptionRecord& subscription : endpoint.subscriptions) {
            clear_subscription(subscription);
        }
        clear_physical_connection(endpoint);
        endpoint.discovered_peers.clear();
        endpoint.open = false;
        endpoint.owner_generation = 0;
        endpoint.device = DeviceHandle{};
        endpoint.close_requested.store(false, std::memory_order_release);
        if (close_work != nullptr) {
            const IoStatus status = cancelled(*close_work) ? IoStatus::cancelled
                                                            : IoStatus::completed;
            finish_work(*close_work, status, EmptyResponse{});
        }
    }

    void stop_scan(WorkRecord& work) {
        if (work.native_abort_sent) return;
        work.scan_stopping = true;
        const int status = ble_gap_disc_cancel();
        if (status == 0) {
            work.native_abort_sent = true;
            // ble_gap_disc_cancel() drops NimBLE's callback registration; unlike
            // duration expiry, it has no later DISC_COMPLETE callback to await.
            complete_scan_operation(work, 0);
        } else if (status == BLE_HS_EALREADY) {
            work.native_abort_sent = true;
        } else {
            remember_partial_failure(work, nimble_status(status));
        }
    }

    void stop_connect(WorkRecord& work) {
        if (work.native_abort_sent) return;
        const int status = ble_gap_conn_cancel();
        if (status != 0 && status != BLE_HS_EALREADY) {
            remember_partial_failure(work, nimble_status(status));
            return;
        }
        work.native_abort_sent = true;
    }

    void stop_connected_peer(EndpointState& endpoint) {
        if (!endpoint.connected || endpoint.disconnect_requested) return;
        const int status = ble_gap_terminate(endpoint.connection_handle,
                                             BLE_ERR_REM_USER_CONN_TERM);
        if (status == 0 || status == BLE_HS_EALREADY) {
            endpoint.disconnect_requested = true;
        } else if (status == BLE_HS_ENOTCONN) {
            endpoint.disconnect_requested = true;
            endpoint.disconnect_local_pending = true;
        }
    }

    void enqueue_local_disconnection(EndpointState& endpoint) {
        if (!endpoint.disconnect_local_pending || !endpoint.connected) return;
        Message message{};
        message.type = MessageType::disconnect;
        message.endpoint = &endpoint;
        message.connection_handle = endpoint.connection_handle;
        // Queue this behind any ATT error events already copied from NimBLE.
        // That ordering preserves their WorkRecord lifetimes before slot reuse.
        if (endpoint.owner->send_best_effort(message)) {
            endpoint.disconnect_local_pending = false;
        }
    }


    void stop_endpoint_activity(EndpointState& endpoint) {
        if (!endpoint.close_requested.load(std::memory_order_acquire)) return;
        if (WorkRecord* scan = endpoint.scan_work.load(std::memory_order_acquire)) {
            stop_scan(*scan);
        }
        if (WorkRecord* connect = endpoint.connect_work.load(std::memory_order_acquire)) {
            stop_connect(*connect);
        }
        stop_connected_peer(endpoint);
    }

    void apply_cancellation(WorkRecord& work) {
        if (!cancelled(work) || work.state != WorkState::native) return;
        if (work.phase == WorkPhase::scan) {
            stop_scan(work);
            return;
        }
        if (work.phase == WorkPhase::connect) stop_connect(work);
    }

    void pump() {
        for (std::size_t index = 0; index < m_endpoint_count; ++index) {
            enqueue_local_disconnection(m_endpoints[index]);
            stop_endpoint_activity(m_endpoints[index]);
            enqueue_local_disconnection(m_endpoints[index]);
        }
        for (WorkRecord& work : m_work) {
            if (work.state == WorkState::native) apply_cancellation(work);
            if (work.state != WorkState::queued) continue;
            begin_queued_work(work);
        }
        for (std::size_t index = 0; index < m_endpoint_count; ++index) {
            EndpointState& endpoint = m_endpoints[index];
            if (close_can_finish(endpoint) || release_can_finish(endpoint)) {
                finish_endpoint_close(endpoint);
            }
        }
    }

    void begin_queued_work(WorkRecord& work) {
        if (work.operation.id == DeviceOperation::close) {
            begin_close(work);
            return;
        }
        if (cancelled(work) && work.operation.id != DeviceOperation::unsubscribe) {
            finish_work(work, IoStatus::cancelled, EmptyResponse{});
            return;
        }
        if (work.operation.id == DeviceOperation::open) {
            begin_open(work);
            return;
        }
        EndpointState& endpoint = *work.endpoint;
        if (endpoint.close_requested.load(std::memory_order_acquire)) {
            finish_work(work, IoStatus::cancelled, EmptyResponse{});
            return;
        }
        const IoStatus lease_status = bind_lease(endpoint, work.operation);
        if (lease_status != IoStatus::completed) {
            finish_work(work, lease_status, EmptyResponse{});
            return;
        }
        switch (work.operation.id) {
        case DeviceOperation::discover: begin_discover(work); break;
        case DeviceOperation::connect: begin_connect(work); break;
        case DeviceOperation::disconnect: begin_disconnect(work); break;
        case DeviceOperation::read:
        case DeviceOperation::write:
        case DeviceOperation::subscribe: begin_attribute_or_subscription(work); break;
        case DeviceOperation::unsubscribe: begin_unsubscribe(work); break;
        default: finish_work(work, IoStatus::unsupported, EmptyResponse{}); break;
        }
    }

    void begin_open(WorkRecord& work) {
        EndpointState& endpoint = *work.endpoint;
        if (endpoint.open) {
            finish_work(work, IoStatus::busy, EmptyResponse{});
            return;
        }
        endpoint.open = true;
        endpoint.owner_generation = work.operation.owner_generation;
        endpoint.device = DeviceHandle{};
        endpoint.close_requested.store(false, std::memory_order_release);
        endpoint.close_work = nullptr;
        endpoint.discovered_peers.clear();
        clear_physical_connection(endpoint);
        for (SubscriptionRecord& subscription : endpoint.subscriptions) {
            clear_subscription(subscription);
        }
        if (cancelled(work)) {
            finish_endpoint_close_without_work(endpoint);
            finish_work(work, IoStatus::cancelled, EmptyResponse{});
            return;
        }
        (void)finish_work(work, IoStatus::completed,
                          OpenResponse{DeviceHandle{}, esp_idf_ble_central_version, false});
    }

    void finish_endpoint_close_without_work(EndpointState& endpoint) noexcept {
        for (SubscriptionRecord& subscription : endpoint.subscriptions) {
            clear_subscription(subscription);
        }
        clear_physical_connection(endpoint);
        endpoint.open = false;
        endpoint.owner_generation = 0;
        endpoint.device = DeviceHandle{};
        endpoint.close_requested.store(false, std::memory_order_release);
    }

    void begin_discover(WorkRecord& work) {
        const auto& request = std::get<BleDiscoverRequest>(work.operation.request);
        if (request.kind == BleDiscoveryKind::scan) {
            begin_scan(work);
            return;
        }
        begin_service_discovery_if_needed(work);
    }

    void begin_scan(WorkRecord& work) {
        EndpointState& endpoint = *work.endpoint;
        if (!native_ready()) {
            finish_work(work, IoStatus::busy, EmptyResponse{});
            return;
        }
        if (m_scan_work != nullptr || m_connect_work != nullptr) return;
        work.scan_peers.clear();
        work.partial_status = IoStatus::completed;
        work.native_abort_sent = false;
        work.scan_stopping = false;
        m_next_scan_token += 2;
        if (m_next_scan_token < 2) m_next_scan_token = 2;
        work.scan_token = m_next_scan_token;
        endpoint.scan_report_state.store(work.scan_token, std::memory_order_release);
        work.phase = WorkPhase::scan;
        work.state = WorkState::native;
        m_scan_work = &work;
        endpoint.scan_work.store(&work, std::memory_order_release);

        std::uint8_t own_address_type = 0;
        int native = ble_hs_id_infer_auto(0, &own_address_type);
        if (native == 0) {
            ble_gap_disc_params parameters{};
            parameters.filter_duplicates = 1;
            parameters.passive = 1;
            native = ble_gap_disc(own_address_type,
                                  static_cast<int32_t>(endpoint.config.scan_duration_ms),
                                  &parameters, &State::gap_event, &endpoint);
        }
        if (native != 0) fail_native_start(work, native);
    }

    void fail_native_start(WorkRecord& work, int native_status) {
        // A procedure start result of EDONE is not an accepted asynchronous
        // operation; in particular, GAP uses it for an already-connected peer.
        const IoStatus status = native_status == BLE_HS_EDONE
                                    ? IoStatus::busy
                                    : nimble_status(native_status);
        clear_native_reservation(work);
        finish_work(work, cancelled(work) ? IoStatus::cancelled : status, EmptyResponse{});
    }

    void clear_native_reservation(WorkRecord& work) noexcept {
        EndpointState& endpoint = *work.endpoint;
        if (endpoint.gatt_work == &work) endpoint.gatt_work = nullptr;
        if (endpoint.disconnect_work == &work) endpoint.disconnect_work = nullptr;
        if (m_scan_work == &work) m_scan_work = nullptr;
        if (m_connect_work == &work) m_connect_work = nullptr;
        if (endpoint.scan_work.load(std::memory_order_relaxed) == &work) {
            endpoint.scan_work.store(nullptr, std::memory_order_release);
        }
        if (endpoint.connect_work.load(std::memory_order_relaxed) == &work) {
            endpoint.connect_work.store(nullptr, std::memory_order_release);
        }
    }

    [[nodiscard]] bool peer_was_discovered(const EndpointState& endpoint,
                                           const BleAddress& peer) const noexcept {
        return std::any_of(endpoint.discovered_peers.begin(),
                           endpoint.discovered_peers.end(),
                           [&](const BleAddress& candidate) {
                               return same_address(candidate, peer);
                           });
    }

    void begin_connect(WorkRecord& work) {
        EndpointState& endpoint = *work.endpoint;
        if (endpoint.connected) {
            finish_work(work, IoStatus::busy, EmptyResponse{});
            return;
        }
        const auto& request = std::get<BleConnectRequest>(work.operation.request);
        if (!peer_was_discovered(endpoint, request.peer)) {
            finish_work(work, IoStatus::invalid_argument, EmptyResponse{});
            return;
        }
        if (!native_ready()) {
            finish_work(work, IoStatus::busy, EmptyResponse{});
            return;
        }
        if (m_scan_work != nullptr || m_connect_work != nullptr) return;

        ble_gap_conn_params parameters{};
        parameters.scan_itvl = 0x0010;
        parameters.scan_window = 0x0010;
        parameters.itvl_min = BLE_GAP_INITIAL_CONN_ITVL_MIN;
        parameters.itvl_max = BLE_GAP_INITIAL_CONN_ITVL_MAX;
        parameters.latency = BLE_GAP_INITIAL_CONN_LATENCY;
        parameters.supervision_timeout = static_cast<std::uint16_t>(
            request.supervision_timeout_ms / 10U);
        parameters.min_ce_len = BLE_GAP_INITIAL_CONN_MIN_CE_LEN;
        parameters.max_ce_len = BLE_GAP_INITIAL_CONN_MAX_CE_LEN;
        const ble_addr_t peer = native_address(request.peer);
        work.phase = WorkPhase::connect;
        work.state = WorkState::native;
        work.native_abort_sent = false;
        work.partial_status = IoStatus::completed;
        m_connect_work = &work;
        endpoint.connect_work.store(&work, std::memory_order_release);
        endpoint.peer = request.peer;

        std::uint8_t own_address_type = 0;
        int native = ble_hs_id_infer_auto(0, &own_address_type);
        if (native == 0) {
            native = ble_gap_connect(own_address_type, &peer,
                                     static_cast<int32_t>(endpoint.config.connect_timeout_ms),
                                     &parameters, &State::gap_event, &endpoint);
        }
        if (native != 0) fail_native_start(work, native);
    }

    void begin_disconnect(WorkRecord& work) {
        EndpointState& endpoint = *work.endpoint;
        const auto& request = std::get<BleDisconnectRequest>(work.operation.request);
        if (!endpoint.connected || !same_address(endpoint.peer, request.peer)) {
            finish_work(work, IoStatus::invalid_argument, EmptyResponse{});
            return;
        }
        if (endpoint.disconnect_work != nullptr) {
            finish_work(work, IoStatus::busy, EmptyResponse{});
            return;
        }
        endpoint.disconnect_work = &work;
        work.phase = WorkPhase::disconnect;
        work.state = WorkState::native;
        stop_connected_peer(endpoint);
        if (!endpoint.disconnect_requested) {
            endpoint.disconnect_work = nullptr;
            finish_work(work, IoStatus::io_failure, EmptyResponse{});
        }
    }

    [[nodiscard]] bool requires_connected_peer(const WorkRecord& work) const noexcept {
        if (work.operation.id == DeviceOperation::read ||
            work.operation.id == DeviceOperation::write) {
            return true;
        }
        if (const auto* subscribe =
                std::get_if<BleSubscribeRequest>(&work.operation.request)) {
            return subscribe->target == BleSubscriptionTarget::characteristic;
        }
        return true;
    }

    [[nodiscard]] std::size_t reserve_subscription(WorkRecord& work) {
        EndpointState& endpoint = *work.endpoint;
        const auto& request = std::get<BleSubscribeRequest>(work.operation.request);
        std::size_t free_index = kNoSubscription;
        for (std::size_t index = 0; index < endpoint.subscriptions.size(); ++index) {
            SubscriptionRecord& subscription = endpoint.subscriptions[index];
            if (!subscription.reserved) {
                if (free_index == kNoSubscription) free_index = index;
                continue;
            }
            if (request.target == BleSubscriptionTarget::characteristic &&
                subscription.target == request.target &&
                same_attribute(subscription.attribute, request.attribute)) {
                return kNoSubscription;
            }
        }
        if (free_index == kNoSubscription) return free_index;
        SubscriptionRecord& subscription = endpoint.subscriptions[free_index];
        clear_subscription(subscription);
        subscription.reserved = true;
        subscription.target = request.target;
        subscription.attribute = request.attribute;
        subscription.maximum_bytes_per_buffer =
            work.operation.budget.maximum_bytes_per_buffer;
        subscription.owner_generation = work.operation.owner_generation;
        subscription.subscription = work.operation.subscription;
        subscription.completions = work.completions;
        work.subscription_index = free_index;
        return free_index;
    }

    void begin_attribute_or_subscription(WorkRecord& work) {
        EndpointState& endpoint = *work.endpoint;
        const auto* subscribe = std::get_if<BleSubscribeRequest>(&work.operation.request);
        if (subscribe && subscribe->target == BleSubscriptionTarget::connection_state) {
            if (reserve_subscription(work) == kNoSubscription) {
                finish_work(work, IoStatus::busy, EmptyResponse{});
                return;
            }
            SubscriptionRecord& subscription =
                endpoint.subscriptions[work.subscription_index];
            subscription.active = true;
            if (cancelled(work)) {
                clear_subscription(subscription);
                work.subscription_index = kNoSubscription;
                finish_work(work, IoStatus::cancelled, EmptyResponse{});
                return;
            }
            finish_work(work, IoStatus::completed,
                        SubscriptionResponse{work.operation.subscription});
            return;
        }
        if (!requires_connected_peer(work)) return;
        if (!endpoint.connected) {
            finish_work(work, IoStatus::busy, EmptyResponse{});
            return;
        }
        if (subscribe && work.subscription_index == kNoSubscription &&
            reserve_subscription(work) == kNoSubscription) {
            finish_work(work, IoStatus::busy, EmptyResponse{});
            return;
        }
        begin_service_discovery_if_needed(work);
    }

    void begin_service_discovery_if_needed(WorkRecord& work) {
        EndpointState& endpoint = *work.endpoint;
        if (!endpoint.connected) {
            finish_work(work, IoStatus::busy, EmptyResponse{});
            return;
        }
        if (has_active_characteristic_subscription(endpoint) &&
            work.operation.id == DeviceOperation::discover) {
            finish_work(work, IoStatus::busy, EmptyResponse{});
            return;
        }
        if (endpoint.attributes_complete) {
            continue_with_attributes(work);
            return;
        }
        if (endpoint.gatt_work != nullptr) return;
        clear_attributes(endpoint);
        work.partial_status = IoStatus::completed;
        work.service_index = 0;
        start_services(work);
    }

    [[nodiscard]] bool has_active_characteristic_subscription(
        const EndpointState& endpoint) const noexcept {
        return std::any_of(endpoint.subscriptions.begin(), endpoint.subscriptions.end(),
                           [](const SubscriptionRecord& subscription) {
                               return subscription.active &&
                                      subscription.target ==
                                          BleSubscriptionTarget::characteristic;
                           });
    }

    void reserve_gatt(WorkRecord& work, WorkPhase phase) {
        work.phase = phase;
        work.state = WorkState::native;
        work.native_abort_sent = false;
        work.endpoint->gatt_work = &work;
    }

    void start_services(WorkRecord& work) {
        reserve_gatt(work, WorkPhase::services);
        const int native = ble_gattc_disc_all_svcs(work.endpoint->connection_handle,
                                                   &State::service_discovered, &work);
        if (native != 0) fail_native_start(work, native);
    }

    void start_characteristics(WorkRecord& work) {
        EndpointState& endpoint = *work.endpoint;
        while (work.service_index < endpoint.service_count) {
            const ServiceRecord& service = endpoint.services[work.service_index];
            if (service.end_handle <= service.start_handle) {
                ++work.service_index;
                continue;
            }
            reserve_gatt(work, WorkPhase::characteristics);
            const int native = ble_gattc_disc_all_chrs(
                endpoint.connection_handle, service.start_handle, service.end_handle,
                &State::characteristic_discovered, &work);
            if (native != 0) fail_native_start(work, native);
            return;
        }
        endpoint.attributes_complete = true;
        continue_with_attributes(work);
    }

    [[nodiscard]] CharacteristicRecord* find_characteristic(
        EndpointState& endpoint, const BleAttribute& attribute) noexcept {
        for (CharacteristicRecord& characteristic : endpoint.characteristics) {
            if (characteristic.value_handle != 0 &&
                same_attribute(characteristic.attribute, attribute)) {
                return &characteristic;
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::size_t characteristic_index(
        EndpointState& endpoint, const CharacteristicRecord& target) const noexcept {
        return static_cast<std::size_t>(&target - endpoint.characteristics.data());
    }

    void remember_partial_failure(WorkRecord& work, IoStatus status) noexcept {
        if (work.partial_status == IoStatus::completed) work.partial_status = status;
    }

    void continue_with_attributes(WorkRecord& work) {
        EndpointState& endpoint = *work.endpoint;
        if (cancelled(work)) {
            finish_work(work, IoStatus::cancelled, EmptyResponse{});
            return;
        }
        if (work.partial_status != IoStatus::completed) {
            endpoint.attributes_complete = false;
            finish_work(work, work.partial_status, EmptyResponse{});
            return;
        }
        if (work.operation.id == DeviceOperation::discover) {
            finish_work(work, IoStatus::completed, discovery_response(work));
            return;
        }
        BleAttribute attribute;
        if (const auto* read = std::get_if<BleReadRequest>(&work.operation.request)) {
            attribute = read->attribute;
        } else if (const auto* write =
                       std::get_if<BleWriteRequest>(&work.operation.request)) {
            attribute = write->attribute;
        } else {
            attribute = std::get<BleSubscribeRequest>(work.operation.request).attribute;
        }
        CharacteristicRecord* const characteristic =
            find_characteristic(endpoint, attribute);
        if (characteristic == nullptr) {
            finish_work(work, IoStatus::invalid_argument, EmptyResponse{});
            return;
        }
        work.characteristic_index = characteristic_index(endpoint, *characteristic);
        if (work.operation.id == DeviceOperation::read) {
            if ((characteristic->properties & BLE_GATT_CHR_PROP_READ) == 0) {
                finish_work(work, IoStatus::unsupported, EmptyResponse{});
                return;
            }
            if (endpoint.gatt_work != nullptr) return;
            work.read_value.clear();
            reserve_gatt(work, WorkPhase::read);
            const int native = ble_gattc_read_long(endpoint.connection_handle,
                                                   characteristic->value_handle, 0,
                                                   &State::attribute_completed, &work);
            if (native != 0) fail_native_start(work, native);
            return;
        }
        if (work.operation.id == DeviceOperation::write) {
            start_write(work, *characteristic);
            return;
        }
        const std::uint8_t updates = BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE;
        if ((characteristic->properties & updates) == 0) {
            finish_work(work, IoStatus::unsupported, EmptyResponse{});
            return;
        }
        if (characteristic->cccd_discovered) {
            start_cccd_subscription(work);
        } else {
            start_descriptor_discovery(work);
        }
    }

    [[nodiscard]] DeviceResponse discovery_response(const WorkRecord& work) const {
        const auto& request = std::get<BleDiscoverRequest>(work.operation.request);
        const EndpointState& endpoint = *work.endpoint;
        BleGattDiscoveryResponse response;
        const std::size_t maximum = std::min<std::size_t>(
            request.maximum_results, endpoint.characteristic_count);
        response.characteristics.reserve(maximum);
        for (std::size_t index = 0;
             index < endpoint.characteristic_count &&
             response.characteristics.size() < request.maximum_results;
             ++index) {
            const CharacteristicRecord& characteristic = endpoint.characteristics[index];
            if (characteristic.value_handle == 0 ||
                !attribute_matches_filter(characteristic.attribute, request.filter)) {
                continue;
            }
            response.characteristics.push_back(BleCharacteristicDescriptor{
                characteristic.attribute,
                (characteristic.properties & BLE_GATT_CHR_PROP_READ) != 0,
                (characteristic.properties & (BLE_GATT_CHR_PROP_WRITE |
                                              BLE_GATT_CHR_PROP_WRITE_NO_RSP)) != 0,
                (characteristic.properties & (BLE_GATT_CHR_PROP_NOTIFY |
                                              BLE_GATT_CHR_PROP_INDICATE)) != 0});
        }
        return response;
    }

    void start_write(WorkRecord& work, const CharacteristicRecord& characteristic) {
        const auto& request = std::get<BleWriteRequest>(work.operation.request);
        const bool without_response = request.mode == BleWriteMode::without_response;
        const std::uint8_t required_property = without_response
                                                   ? BLE_GATT_CHR_PROP_WRITE_NO_RSP
                                                   : BLE_GATT_CHR_PROP_WRITE;
        if ((characteristic.properties & required_property) == 0) {
            finish_work(work, IoStatus::unsupported, EmptyResponse{});
            return;
        }
        EndpointState& endpoint = *work.endpoint;
        const std::uint16_t mtu = ble_att_mtu(endpoint.connection_handle);
        if (mtu < 3) {
            finish_work(work, IoStatus::device_lost, EmptyResponse{});
            return;
        }
        if (endpoint.gatt_work != nullptr) return;
        if (without_response && request.value->size() > static_cast<std::size_t>(mtu - 3)) {
            finish_work(work, IoStatus::invalid_argument, EmptyResponse{});
            return;
        }
        reserve_gatt(work, WorkPhase::write);
        int native = 0;
        if (without_response) {
            native = ble_gattc_write_no_rsp_flat(
                endpoint.connection_handle, characteristic.value_handle,
                request.value->data(), static_cast<std::uint16_t>(request.value->size()));
            clear_native_reservation(work);
            const bool succeeded = native == 0;
            const IoStatus status = cancelled(work) ? IoStatus::cancelled
                                      : succeeded ? IoStatus::completed
                                                  : nimble_status(native);
            finish_work(work, status,
                        succeeded ? DeviceResponse{BleWriteResponse{request.value->size()}}
                                  : DeviceResponse{EmptyResponse{}});
            return;
        }
        if (request.value->size() > static_cast<std::size_t>(mtu - 3)) {
            struct os_mbuf* value = ble_hs_mbuf_from_flat(
                request.value->data(), static_cast<std::uint16_t>(request.value->size()));
            if (value == nullptr) {
                fail_native_start(work, BLE_HS_ENOMEM);
                return;
            }
            native = ble_gattc_write_long(endpoint.connection_handle,
                                          characteristic.value_handle, 0, value,
                                          &State::attribute_completed, &work);
        } else {
            native = ble_gattc_write_flat(
                endpoint.connection_handle, characteristic.value_handle,
                request.value->data(), static_cast<std::uint16_t>(request.value->size()),
                &State::attribute_completed, &work);
        }
        if (native != 0) fail_native_start(work, native);
    }

    [[nodiscard]] std::uint16_t characteristic_end_handle(
        const EndpointState& endpoint, std::size_t index) const noexcept {
        const CharacteristicRecord& characteristic = endpoint.characteristics[index];
        for (std::size_t next = index + 1; next < endpoint.characteristic_count; ++next) {
            const CharacteristicRecord& candidate = endpoint.characteristics[next];
            if (candidate.definition_handle > characteristic.definition_handle &&
                same_uuid(candidate.attribute.service, characteristic.attribute.service)) {
                return static_cast<std::uint16_t>(candidate.definition_handle - 1U);
            }
        }
        for (std::size_t service = 0; service < endpoint.service_count; ++service) {
            if (same_uuid(endpoint.services[service].uuid,
                          characteristic.attribute.service)) {
                return endpoint.services[service].end_handle;
            }
        }
        return characteristic.value_handle;
    }

    void start_descriptor_discovery(WorkRecord& work) {
        EndpointState& endpoint = *work.endpoint;
        if (endpoint.gatt_work != nullptr) return;
        CharacteristicRecord& characteristic =
            endpoint.characteristics[work.characteristic_index];
        characteristic.cccd_handle = 0;
        characteristic.cccd_discovered = false;
        work.partial_status = IoStatus::completed;
        reserve_gatt(work, WorkPhase::descriptors);
        const std::uint16_t end_handle =
            characteristic_end_handle(endpoint, work.characteristic_index);
        const int native = ble_gattc_disc_all_dscs(
            endpoint.connection_handle, characteristic.value_handle, end_handle,
            &State::descriptor_discovered, &work);
        if (native != 0) fail_native_start(work, native);
    }

    void start_cccd_subscription(WorkRecord& work) {
        EndpointState& endpoint = *work.endpoint;
        CharacteristicRecord& characteristic =
            endpoint.characteristics[work.characteristic_index];
        if (!characteristic.cccd_discovered) {
            start_descriptor_discovery(work);
            return;
        }
        if (characteristic.cccd_handle == 0) {
            finish_work(work, IoStatus::unsupported, EmptyResponse{});
            return;
        }
        SubscriptionRecord& subscription =
            endpoint.subscriptions[work.subscription_index];
        subscription.value_handle = characteristic.value_handle;
        subscription.cccd_handle = characteristic.cccd_handle;
        const std::array<std::uint8_t, 2> value{
            static_cast<std::uint8_t>(
                (characteristic.properties & BLE_GATT_CHR_PROP_NOTIFY) != 0 ? 0x01 : 0x02),
            0x00};
        start_cccd_write(work, value, WorkPhase::subscribe_cccd);
    }

    void start_cccd_write(WorkRecord& work, std::span<const std::uint8_t> value,
                          WorkPhase phase) {
        EndpointState& endpoint = *work.endpoint;
        if (endpoint.gatt_work != nullptr) return;
        SubscriptionRecord& subscription =
            endpoint.subscriptions[work.subscription_index];
        reserve_gatt(work, phase);
        const int native = ble_gattc_write_flat(
            endpoint.connection_handle, subscription.cccd_handle, value.data(),
            static_cast<std::uint16_t>(value.size()), &State::attribute_completed, &work);
        if (native != 0) {
            if (phase == WorkPhase::unsubscribe_cccd) {
                clear_native_reservation(work);
                complete_unsubscribe(work, native);
                return;
            }
            if (phase == WorkPhase::subscribe_rollback) {
                clear_native_reservation(work);
                // Enable already completed, so rollback failure must not leave
                // the peer's notification stream associated with a valid lease.
                if (native != BLE_HS_ENOTCONN) request_endpoint_close(endpoint);
                finish_work(work, IoStatus::cancelled, EmptyResponse{});
                return;
            }
            fail_native_start(work, native);
        }
    }

    void begin_unsubscribe(WorkRecord& work) {
        EndpointState& endpoint = *work.endpoint;
        const DeviceHandle handle =
            std::get<UnsubscribeRequest>(work.operation.request).subscription;
        for (std::size_t index = 0; index < endpoint.subscriptions.size(); ++index) {
            SubscriptionRecord& subscription = endpoint.subscriptions[index];
            if (!subscription.reserved || !subscription.active ||
                subscription.subscription != handle) {
                continue;
            }
            work.subscription_index = index;
            subscription.retiring.store(true, std::memory_order_release);
            if (subscription.target == BleSubscriptionTarget::connection_state ||
                !endpoint.connected) {
                clear_subscription(subscription);
                work.subscription_index = kNoSubscription;
                finish_work(work, cancelled(work) ? IoStatus::cancelled : IoStatus::completed,
                            EmptyResponse{});
                return;
            }
            if (endpoint.gatt_work != nullptr) return;
            const std::array<std::uint8_t, 2> disable{0x00, 0x00};
            start_cccd_write(work, disable, WorkPhase::unsubscribe_cccd);
            return;
        }
        finish_work(work, IoStatus::invalid_argument, EmptyResponse{});
    }


    void process_service(const Message& message) {
        WorkRecord& work = *message.work;
        if (!valid_native_work(work, WorkPhase::services, message)) return;
        if (message.status == 0) {
            if (work.endpoint->service_count >= work.endpoint->services.size()) {
                remember_partial_failure(work, IoStatus::memory_exhausted);
                return;
            }
            const auto uuid = portable_uuid(message.uuid);
            if (!uuid) {
                remember_partial_failure(work, IoStatus::unsupported);
                return;
            }
            ServiceRecord& service = work.endpoint->services[work.endpoint->service_count++];
            service = ServiceRecord{*uuid, message.handle, message.end_handle};
            return;
        }
        clear_native_reservation(work);
        if (message.status != BLE_HS_EDONE) {
            if (cancelled(work)) finish_work(work, IoStatus::cancelled, EmptyResponse{});
            else finish_work(work, nimble_status(message.status), EmptyResponse{});
            return;
        }
        if (cancelled(work)) {
            finish_work(work, IoStatus::cancelled, EmptyResponse{});
            return;
        }
        work.service_index = 0;
        start_characteristics(work);
    }

    void process_characteristic(const Message& message) {
        WorkRecord& work = *message.work;
        EndpointState& endpoint = *work.endpoint;
        if (!valid_native_work(work, WorkPhase::characteristics, message)) return;
        if (message.status == 0) {
            if (endpoint.characteristic_count >= endpoint.characteristics.size()) {
                remember_partial_failure(work, IoStatus::memory_exhausted);
                return;
            }
            const auto characteristic_uuid = portable_uuid(message.uuid);
            if (!characteristic_uuid || work.service_index >= endpoint.service_count) {
                remember_partial_failure(work, IoStatus::unsupported);
                return;
            }
            CharacteristicRecord& characteristic =
                endpoint.characteristics[endpoint.characteristic_count++];
            characteristic = CharacteristicRecord{};
            characteristic.attribute = BleAttribute{
                endpoint.services[work.service_index].uuid, *characteristic_uuid};
            characteristic.definition_handle = message.handle;
            characteristic.value_handle = message.characteristic_value_handle;
            characteristic.properties = message.properties;
            return;
        }
        clear_native_reservation(work);
        if (message.status != BLE_HS_EDONE) {
            if (cancelled(work)) finish_work(work, IoStatus::cancelled, EmptyResponse{});
            else finish_work(work, nimble_status(message.status), EmptyResponse{});
            return;
        }
        if (cancelled(work)) finish_work(work, IoStatus::cancelled, EmptyResponse{});
        else {
            ++work.service_index;
            start_characteristics(work);
        }
    }

    void process_descriptor(const Message& message) {
        WorkRecord& work = *message.work;
        EndpointState& endpoint = *work.endpoint;
        if (!valid_native_work(work, WorkPhase::descriptors, message)) return;
        CharacteristicRecord& characteristic =
            endpoint.characteristics[work.characteristic_index];
        if (message.status == 0) {
            const auto uuid = portable_uuid(message.uuid);
            if (!uuid) {
                remember_partial_failure(work, IoStatus::unsupported);
                return;
            }
            if (same_uuid(*uuid, BleUuid::from_uint16(BLE_GATT_DSC_CLT_CFG_UUID16))) {
                if (characteristic.cccd_handle != 0) {
                    remember_partial_failure(work, IoStatus::io_failure);
                } else {
                    characteristic.cccd_handle = message.handle;
                }
            }
            return;
        }
        clear_native_reservation(work);
        if (message.status != BLE_HS_EDONE) {
            characteristic.cccd_handle = 0;
            characteristic.cccd_discovered = false;
            finish_work(work, cancelled(work) ? IoStatus::cancelled
                                               : nimble_status(message.status),
                        EmptyResponse{});
            return;
        }
        characteristic.cccd_discovered = true;
        if (work.partial_status != IoStatus::completed) {
            characteristic.cccd_handle = 0;
            characteristic.cccd_discovered = false;
            finish_work(work, cancelled(work) ? IoStatus::cancelled : work.partial_status,
                        EmptyResponse{});
            return;
        }
        if (cancelled(work)) finish_work(work, IoStatus::cancelled, EmptyResponse{});
        else start_cccd_subscription(work);
    }

    [[nodiscard]] bool valid_native_work(const WorkRecord& work, WorkPhase phase,
                                         const Message& message) const noexcept {
        return work.state == WorkState::native && work.phase == phase &&
               work.endpoint == message.endpoint &&
               work.endpoint->gatt_work == &work &&
               work.endpoint->connected &&
               work.endpoint->connection_handle == message.connection_handle;
    }

    void process_attribute(const Message& message) {
        WorkRecord& work = *message.work;
        EndpointState& endpoint = *work.endpoint;
        const WorkPhase phase = work.phase;
        if ((phase != WorkPhase::read && phase != WorkPhase::write &&
             phase != WorkPhase::subscribe_cccd && phase != WorkPhase::subscribe_rollback &&
             phase != WorkPhase::unsubscribe_cccd) ||
            !valid_native_work(work, phase, message)) {
            return;
        }
        if (phase == WorkPhase::read && message.status == 0) {
            process_read_chunk(work, message);
            return;
        }
        clear_native_reservation(work);
        if (phase == WorkPhase::read) {
            IoStatus status = nimble_status(message.status);
            if (message.status == BLE_HS_EDONE) {
                status = work.partial_status;
                if (status == IoStatus::completed) {
                    if (cancelled(work)) {
                        finish_work(work, IoStatus::cancelled, EmptyResponse{});
                    } else {
                        finish_work(work, IoStatus::completed,
                                    BleReadResponse{
                                        std::get<BleReadRequest>(work.operation.request).attribute,
                                        make_immutable_bytes(work.read_value)});
                    }
                    return;
                }
            }
            finish_work(work, cancelled(work) ? IoStatus::cancelled : status,
                        EmptyResponse{});
            return;
        }
        if (phase == WorkPhase::write) {
            if (message.status == 0 && !cancelled(work)) {
                finish_work(work, IoStatus::completed,
                            BleWriteResponse{std::get<BleWriteRequest>(work.operation.request)
                                                 .value->size()});
            } else {
                finish_work(work, cancelled(work) ? IoStatus::cancelled
                                                   : nimble_status(message.status),
                            EmptyResponse{});
            }
            return;
        }
        if (phase == WorkPhase::subscribe_cccd) {
            complete_cccd_subscription(work, message.status);
            return;
        }
        if (phase == WorkPhase::subscribe_rollback) {
            if (message.status != 0 && message.status != BLE_HS_ENOTCONN) {
                request_endpoint_close(endpoint);
            }
            finish_work(work, IoStatus::cancelled, EmptyResponse{});
            return;
        }
        complete_unsubscribe(work, message.status);
    }

    void process_read_chunk(WorkRecord& work, const Message& message) {
        if (message.copy_failed) {
            remember_partial_failure(work, IoStatus::memory_exhausted);
            return;
        }
        if (message.offset != work.read_value.size()) {
            remember_partial_failure(work, IoStatus::io_failure);
            return;
        }
        const std::size_t new_size = work.read_value.size() + message.length;
        const std::size_t limit = std::min<std::size_t>(
            esp_idf_ble_central_max_attribute_bytes,
            work.operation.budget.maximum_bytes_per_buffer);
        if (new_size > limit) {
            remember_partial_failure(work, IoStatus::memory_exhausted);
            return;
        }
        work.read_value.insert(work.read_value.end(), message.bytes.begin(),
                               message.bytes.begin() + message.length);
    }

    void complete_cccd_subscription(WorkRecord& work, int native_status) {
        EndpointState& endpoint = *work.endpoint;
        SubscriptionRecord& subscription =
            endpoint.subscriptions[work.subscription_index];
        if (native_status != 0) {
            if (native_status != BLE_HS_ENOTCONN && cancelled(work)) {
                request_endpoint_close(endpoint);
            }
            finish_work(work, cancelled(work) ? IoStatus::cancelled
                                               : nimble_status(native_status),
                        EmptyResponse{});
            return;
        }
        if (cancelled(work)) {
            const std::array<std::uint8_t, 2> disable{0x00, 0x00};
            start_cccd_write(work, disable, WorkPhase::subscribe_rollback);
            return;
        }
        subscription.active = true;
        subscription.retiring.store(false, std::memory_order_release);
        finish_work(work, IoStatus::completed,
                    SubscriptionResponse{work.operation.subscription});
    }

    void complete_unsubscribe(WorkRecord& work, int native_status) {
        EndpointState& endpoint = *work.endpoint;
        SubscriptionRecord& subscription =
            endpoint.subscriptions[work.subscription_index];
        clear_subscription(subscription);
        work.subscription_index = kNoSubscription;
        if (native_status != 0 && native_status != BLE_HS_ENOTCONN) {
            request_endpoint_close(endpoint);
        }
        const IoStatus status = cancelled(work)
                                    ? IoStatus::cancelled
                                    : (native_status == 0 || native_status == BLE_HS_ENOTCONN)
                                          ? IoStatus::completed
                                          : nimble_status(native_status);
        finish_work(work, status, EmptyResponse{});
    }


    void process_advertisement(const Message& message) {
        EndpointState& endpoint = *message.endpoint;
        WorkRecord* const work = m_scan_work;
        if (work == nullptr || work->endpoint != &endpoint ||
            work->state != WorkState::native || work->phase != WorkPhase::scan ||
            work->scan_stopping || message.scan_token != work->scan_token) {
            return;
        }
        if (message.copy_failed || message.length > esp_idf_ble_central_max_advertisement_bytes) {
            remember_partial_failure(*work, IoStatus::memory_exhausted);
            stop_scan(*work);
            return;
        }
        ble_addr_t native{};
        native.type = message.address_type;
        std::copy(message.address.begin(), message.address.end(), std::begin(native.val));
        const BleAddress address = portable_address(native);
        const auto existing = std::find_if(
            work->scan_peers.begin(), work->scan_peers.end(),
            [&](const BlePeer& peer) { return same_address(peer.address, address); });
        if (existing != work->scan_peers.end()) return;
        if (message.length > work->operation.budget.maximum_bytes_per_buffer) {
            remember_partial_failure(*work, IoStatus::memory_exhausted);
            stop_scan(*work);
            return;
        }
        const std::uint32_t requested =
            std::get<BleDiscoverRequest>(work->operation.request).maximum_results;
        if (work->scan_peers.size() >= esp_idf_ble_central_max_scan_results &&
            work->scan_peers.size() < requested) {
            remember_partial_failure(*work, IoStatus::memory_exhausted);
            stop_scan(*work);
            return;
        }
        if (work->scan_peers.size() >= requested) {
            stop_scan(*work);
            return;
        }
        work->scan_peers.push_back(BlePeer{
            address, message.rssi,
            make_immutable_bytes(std::span<const std::uint8_t>(
                message.bytes.data(), message.length))});
        if (work->scan_peers.size() >= requested) {
            stop_scan(*work);
        } else if (work->scan_peers.size() >= esp_idf_ble_central_max_scan_results) {
            remember_partial_failure(*work, IoStatus::memory_exhausted);
            stop_scan(*work);
        }
    }

    void process_scan_complete(const Message& message) {
        WorkRecord* const work = m_scan_work;
        if (work == nullptr || work->endpoint != message.endpoint ||
            work->state != WorkState::native || work->phase != WorkPhase::scan) {
            return;
        }
        complete_scan_operation(*work, message.status);
    }

    void complete_scan_operation(WorkRecord& work, int native_status) {
        clear_native_reservation(work);
        const std::uint32_t report_state =
            work.endpoint->scan_report_state.load(std::memory_order_acquire);
        if (report_state == (work.scan_token | 1U)) {
            remember_partial_failure(work, IoStatus::memory_exhausted);
        }
        if (native_status != 0) {
            remember_partial_failure(work, nimble_status(native_status));
        }
        if (cancelled(work)) {
            work.endpoint->discovered_peers.clear();
            finish_work(work, IoStatus::cancelled, EmptyResponse{});
            return;
        }
        if (work.partial_status != IoStatus::completed) {
            work.endpoint->discovered_peers.clear();
            finish_work(work, work.partial_status, EmptyResponse{});
            return;
        }
        EndpointState& endpoint = *work.endpoint;
        endpoint.discovered_peers.clear();
        endpoint.discovered_peers.reserve(work.scan_peers.size());
        for (const BlePeer& peer : work.scan_peers) {
            endpoint.discovered_peers.push_back(peer.address);
        }
        BleScanResponse response;
        response.peers = std::move(work.scan_peers);
        finish_work(work, IoStatus::completed, std::move(response));
    }

    void process_connect(const Message& message) {
        EndpointState& endpoint = *message.endpoint;
        WorkRecord* const work = m_connect_work;
        if (work == nullptr || work->endpoint != &endpoint ||
            work->state != WorkState::native || work->phase != WorkPhase::connect) {
            return;
        }
        clear_native_reservation(*work);
        if (message.status != 0) {
            endpoint.connected = false;
            const IoStatus status = cancelled(*work) ? IoStatus::cancelled
                                                      : nimble_status(message.status);
            finish_work(*work, status, EmptyResponse{});
            return;
        }
        endpoint.connected = true;
        endpoint.connection_handle = message.connection_handle;
        endpoint.disconnect_requested = false;
        endpoint.paired = message.paired;
        clear_attributes(endpoint);
        clear_characteristic_subscriptions(endpoint);
        if (message.copy_failed) {
            // The controller created the link even though its descriptor could
            // not be copied. Keep it tracked until NimBLE confirms termination.
            stop_connected_peer(endpoint);
            finish_work(*work, cancelled(*work) ? IoStatus::cancelled : IoStatus::io_failure,
                        EmptyResponse{});
            return;
        }
        if (cancelled(*work) ||
            endpoint.close_requested.load(std::memory_order_acquire)) {
            stop_connected_peer(endpoint);
            finish_work(*work, IoStatus::cancelled, EmptyResponse{});
            return;
        }
        publish_connection(endpoint, true);
        finish_work(*work, IoStatus::completed,
                    BleConnectionResponse{endpoint.peer, true, endpoint.paired});
    }

    void process_disconnect(const Message& message) {
        EndpointState& endpoint = *message.endpoint;
        if (!endpoint.connected || endpoint.connection_handle != message.connection_handle) {
            return;
        }
        WorkRecord* const disconnect_work = endpoint.disconnect_work;
        clear_physical_connection(endpoint);
        publish_connection(endpoint, false);
        for (WorkRecord& work : m_work) {
            if (work.state == WorkState::native && work.endpoint == &endpoint &&
                &work != disconnect_work) {
                finish_work(work, cancelled(work) ? IoStatus::cancelled : IoStatus::device_lost,
                            EmptyResponse{});
            }
        }
        endpoint.disconnect_work = nullptr;
        if (disconnect_work != nullptr) {
            finish_work(*disconnect_work,
                        cancelled(*disconnect_work) ? IoStatus::cancelled : IoStatus::completed,
                        EmptyResponse{});
        }
    }
    void process_termination_failure(const Message& message) {
        EndpointState& endpoint = *message.endpoint;
        if (!endpoint.connected || endpoint.connection_handle != message.connection_handle) {
            return;
        }
        endpoint.disconnect_requested = false;
        WorkRecord* const disconnect_work = endpoint.disconnect_work;
        endpoint.disconnect_work = nullptr;
        if (disconnect_work != nullptr) {
            finish_work(*disconnect_work,
                        cancelled(*disconnect_work) ? IoStatus::cancelled
                                                    : nimble_status(message.status),
                        EmptyResponse{});
        }
    }


    void publish_connection(EndpointState& endpoint, bool connected) {
        if (m_shutting_down) return;
        for (SubscriptionRecord& subscription : endpoint.subscriptions) {
            if (!subscription.active ||
                subscription.target != BleSubscriptionTarget::connection_state ||
                subscription.retiring.load(std::memory_order_acquire)) {
                continue;
            }
            const auto completions = subscription.completions.lock();
            if (!completions) continue;
            (void)completions->publish(subscription.owner_generation,
                                       subscription.subscription,
                                       BleConnectionResponse{endpoint.peer, connected,
                                                             connected && endpoint.paired});
        }
    }

    void process_notification(const Message& message) {
        EndpointState& endpoint = *message.endpoint;
        if (!endpoint.connected || endpoint.connection_handle != message.connection_handle ||
            message.copy_failed) {
            return;
        }
        for (SubscriptionRecord& subscription : endpoint.subscriptions) {
            if (!subscription.active ||
                subscription.target != BleSubscriptionTarget::characteristic ||
                subscription.retiring.load(std::memory_order_acquire) ||
                subscription.value_handle != message.handle ||
                message.length > subscription.maximum_bytes_per_buffer) {
                continue;
            }
            const auto completions = subscription.completions.lock();
            if (!completions) continue;
            // Full application queues and oversized packets apply bounded
            // backpressure. No notification is truncated or requeued here.
            (void)completions->publish(
                subscription.owner_generation, subscription.subscription,
                BleReadResponse{subscription.attribute,
                                make_immutable_bytes(std::span<const std::uint8_t>(
                                    message.bytes.data(), message.length))});
        }
    }

    void process_encryption_change(const Message& message) {
        EndpointState& endpoint = *message.endpoint;
        if (!endpoint.connected || endpoint.connection_handle != message.connection_handle ||
            message.status != 0) {
            return;
        }
        const bool paired_changed = endpoint.paired != message.paired;
        endpoint.paired = message.paired;
        if (paired_changed) publish_connection(endpoint, true);
    }

    void process_host_reset(const Message&) {
        m_synchronized = false;
        m_scan_work = nullptr;
        m_connect_work = nullptr;
        for (std::size_t index = 0; index < m_endpoint_count; ++index) {
            EndpointState& endpoint = m_endpoints[index];
            const bool was_connected = endpoint.connected;
            for (WorkRecord& work : m_work) {
                if (work.state != WorkState::native || work.endpoint != &endpoint ||
                    work.operation.id == DeviceOperation::close) {
                    continue;
                }
                finish_work(work, cancelled(work) ? IoStatus::cancelled : IoStatus::device_lost,
                            EmptyResponse{});
            }
            clear_physical_connection(endpoint);
            endpoint.scan_work.store(nullptr, std::memory_order_release);
            endpoint.connect_work.store(nullptr, std::memory_order_release);
            endpoint.disconnect_work = nullptr;
            endpoint.gatt_work = nullptr;
            if (was_connected) publish_connection(endpoint, false);
        }
    }

    void admit_operation(const Message& message) noexcept {
        WorkRecord* const work = message.work;
        if (work == nullptr) return;
        portENTER_CRITICAL(&m_mutex);
        if (work->state == WorkState::submitted &&
            work->submission_token == message.submission_token) {
            work->state = WorkState::queued;
        }
        portEXIT_CRITICAL(&m_mutex);
    }

    void process_message(const Message& message) {
        switch (message.type) {
        case MessageType::operation: admit_operation(message); break;
        case MessageType::advertisement: process_advertisement(message); break;
        case MessageType::scan_complete: process_scan_complete(message); break;
        case MessageType::connect: process_connect(message); break;
        case MessageType::disconnect: process_disconnect(message); break;
        case MessageType::termination_failure: process_termination_failure(message); break;
        case MessageType::notification: process_notification(message); break;
        case MessageType::encryption_change: process_encryption_change(message); break;
        case MessageType::service: process_service(message); break;
        case MessageType::characteristic: process_characteristic(message); break;
        case MessageType::descriptor: process_descriptor(message); break;
        case MessageType::attribute: process_attribute(message); break;
        case MessageType::host_sync:
            m_synchronized = message.status == 0;
            break;
        case MessageType::host_reset: process_host_reset(message); break;
        case MessageType::wake:
        case MessageType::stop:
            break;
        }
    }

    [[nodiscard]] bool progress_wait_needed() const noexcept {
        for (std::size_t index = 0; index < m_endpoint_count; ++index) {
            const EndpointState& endpoint = m_endpoints[index];
            if (endpoint.close_requested.load(std::memory_order_acquire) ||
                endpoint.disconnect_local_pending) {
                return true;
            }
        }
        for (const WorkRecord& work : m_work) {
            if (work.state != WorkState::native || work.native_abort_sent ||
                (work.phase != WorkPhase::scan && work.phase != WorkPhase::connect)) {
                continue;
            }
            if (cancelled(work) || (work.phase == WorkPhase::scan && work.scan_stopping)) {
                return true;
            }
        }
        return false;
    }

    void run_worker() {
        bool stop_requested = false;
        for (;;) {
            Message message{};
            const TickType_t wait_ticks = (stop_requested || progress_wait_needed())
                                              ? pdMS_TO_TICKS(100)
                                              : portMAX_DELAY;
            if (xQueueReceive(m_worker_queue, &message, wait_ticks) == pdTRUE) {
                if (message.type == MessageType::stop) {
                    stop_requested = true;
                    m_shutting_down = true;
                    for (std::size_t index = 0; index < m_endpoint_count; ++index) {
                        request_endpoint_close(m_endpoints[index]);
                    }
                } else {
                    process_message(message);
                }
            }
            pump();
            if (stop_requested && native_quiescent()) return;
        }
    }

    [[nodiscard]] bool stop() noexcept {
        if (m_stopped) return false;
        m_stopped = true;
        portENTER_CRITICAL(&m_mutex);
        m_accepting = false;
        for (WorkRecord& work : m_work) {
            if (work.state != WorkState::free) {
                work.retirement_requested.store(true, std::memory_order_release);
            }
        }
        for (std::size_t index = 0; index < m_endpoint_count; ++index) {
            m_endpoints[index].close_requested.store(true, std::memory_order_release);
        }
        portEXIT_CRITICAL(&m_mutex);

        const TickType_t wait_ticks = detail::worker_shutdown_ticks(kShutdownIoTimeoutMs);
        if (m_worker_started) {
            Message message{};
            message.type = MessageType::stop;
            if (xQueueSend(m_worker_queue, &message, wait_ticks) != pdTRUE ||
                xSemaphoreTake(m_worker_done, wait_ticks) != pdTRUE) {
                ESP_LOGE(kLogTag, "BLE central worker join timed out; retaining native resources");
                return false;
            }
            m_callbacks_accepting.store(false, std::memory_order_release);
            m_worker_started = false;
        }
        if (m_host_task_started) {
            const int stop_status = nimble_port_stop();
            if (stop_status != 0) {
                ESP_LOGE(kLogTag, "stopping NimBLE host failed: %d", stop_status);
                return false;
            }
            if (xSemaphoreTake(m_host_done, detail::worker_shutdown_ticks(0)) != pdTRUE) {
                ESP_LOGE(kLogTag, "NimBLE host task did not retire; retaining native resources");
                return false;
            }
            m_host_task_started = false;
        }
        if (m_port_initialized) {
            const esp_err_t status = nimble_port_deinit();
            if (status != ESP_OK) {
                ESP_LOGE(kLogTag, "deinitializing NimBLE controller and host: %s",
                         esp_err_to_name(status));
                return false;
            }
            m_port_initialized = false;
        }
        release_process_stack();
        delete_native_objects();
        return true;
    }

    std::array<EndpointState, esp_idf_ble_central_max_endpoints> m_endpoints{};
    std::size_t m_endpoint_count = 0;
    std::array<WorkRecord, esp_idf_ble_central_max_pending_operations> m_work{};
    std::array<std::uint8_t, kWorkerQueueLength * sizeof(Message)> m_queue_storage{};
    StaticQueue_t m_queue_control{};
    QueueHandle_t m_worker_queue = nullptr;
    StaticSemaphore_t m_worker_done_control{};
    StaticSemaphore_t m_host_done_control{};
    SemaphoreHandle_t m_worker_done = nullptr;
    SemaphoreHandle_t m_host_done = nullptr;
    alignas(StackType_t) std::array<std::uint8_t, kWorkerStackBytes> m_worker_stack{};
    StaticTask_t m_worker_control{};
    TaskHandle_t m_worker = nullptr;
    portMUX_TYPE m_mutex = portMUX_INITIALIZER_UNLOCKED;
    WorkRecord* m_scan_work = nullptr;
    WorkRecord* m_connect_work = nullptr;
    std::uint32_t m_next_scan_token = 0;
    std::uint32_t m_next_submission_token = 0;
    bool m_accepting = true;
    std::atomic<bool> m_callbacks_accepting{true};
    bool m_synchronized = false;
    bool m_shutting_down = false;
    bool m_stopped = false;
    bool m_worker_started = false;
    bool m_host_task_started = false;
    bool m_port_initialized = false;
    bool m_host_callbacks_installed = false;
    bool m_process_stack_claimed = false;
};

EspIdfBleCentralBackend::EspIdfBleCentralBackend(
    std::span<const EspBleCentralEndpointConfig> endpoints) {
    if (!endpoints.empty()) m_state = std::make_unique<State>(endpoints);
}

EspIdfBleCentralBackend::~EspIdfBleCentralBackend() {
    if (!m_state) return;
    if (m_state->stop()) m_state.reset();
    else (void)m_state.release();
}

std::span<const DeviceOperation> EspIdfBleCentralBackend::operations() const noexcept {
    return kImplementedOperations;
}

IoStatus EspIdfBleCentralBackend::start_operation(
    const BackendOperation& operation,
    std::weak_ptr<PeripheralCompletionSink> completions) {
    if (!m_state) return IoStatus::invalid_argument;
    return m_state->start(operation, std::move(completions));
}

IoStatus EspIdfBleCentralBackend::cancel_operation(const BackendOperation& operation) {
    if (!m_state) return IoStatus::invalid_argument;
    return m_state->cancel(operation);
}

void EspIdfBleCentralBackend::release_application(std::uint64_t owner_generation) {
    if (m_state) m_state->release(owner_generation);
}

EspIdfBleCentralRegistry::EspIdfBleCentralRegistry(
    PeripheralRegistry& registry,
    std::span<const EspBleCentralEndpointConfig> endpoints)
    : m_registry(registry) {
    if (endpoints.empty()) return;
    m_ble_central = std::make_shared<EspIdfBleCentralBackend>(endpoints);
    for (const EspBleCentralEndpointConfig& endpoint : endpoints) {
        m_registry.register_endpoint(
            {endpoint.logical_name, esp_idf_ble_central_version, false},
            EndpointAccess::application, m_ble_central);
    }
}

} // namespace mb
#endif // CONFIG_MB_BLE_CENTRAL
