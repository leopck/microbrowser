#include "esp_idf.h"

#include "microbrowser/vm.h"

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <inttypes.h>
#include <limits>
#include <new>
#include <span>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mb {
namespace {

constexpr char kLogTag[] = "microbrowser";
constexpr char kStorageNamespace[] = "mb";
constexpr std::size_t kRequestLimit = 8;
constexpr std::size_t kMaxScriptResponseBytes = 64U * 1024U;
constexpr std::size_t kMaxContentResponseBytes = 2U * 1024U * 1024U;
constexpr std::size_t kMaxScriptUrlBytes = 2047;
constexpr std::size_t kMaxContentUrlBytes = 8192;
constexpr std::size_t kMaxContentBodyBytes = 4096;
constexpr int kMaxScriptRedirects = 3;
constexpr int kMaxContentRedirects = 5;
constexpr std::size_t kStorageRecords = 64;
constexpr std::size_t kMaxStorageKeyBytes = 128;
constexpr std::size_t kMaxStorageStringBytes = 4096;
constexpr std::size_t kMaxStorageBinaryBytes = 4096;
constexpr std::size_t kStorageHeaderBytes = 6;
constexpr std::size_t kMaxStorageRecordBytes = kStorageHeaderBytes + kMaxStorageKeyBytes + kMaxStorageStringBytes;
constexpr uint8_t kStorageFormatVersion = 1;
constexpr UBaseType_t kHttpWorkerPriority = 4;
constexpr uint32_t kHttpWorkerStackBytes = 12 * 1024;
constexpr int kHttpTimeoutMs = 10'000;
constexpr uint32_t kDhcpTimeoutMs = 10'000;
constexpr uint32_t kShutdownTimeoutMs = 45'000;

void check_esp(esp_err_t error, std::string_view operation) {
    if (error == ESP_OK) return;
    std::string message(operation);
    message += ": ";
    message += esp_err_to_name(error);
    throw std::runtime_error(message);
}

void check_esp_without_abort(esp_err_t error, std::string_view operation) {
    if (error == ESP_OK) return;
    ESP_LOGE(kLogTag, "%.*s: %s", int(operation.size()), operation.data(), esp_err_to_name(error));
}

uint16_t read_u16(std::span<const uint8_t> bytes, std::size_t offset) {
    return uint16_t(bytes[offset]) | (uint16_t(bytes[offset + 1]) << 8U);
}

void append_u16(std::vector<uint8_t>& output, uint16_t value) {
    output.push_back(uint8_t(value));
    output.push_back(uint8_t(value >> 8U));
}

void append_u32(std::vector<uint8_t>& output, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) output.push_back(uint8_t(value >> shift));
}

uint64_t read_u64(std::span<const uint8_t> bytes) {
    uint64_t value = 0;
    for (unsigned byte = 0; byte < 8; ++byte) value |= uint64_t(bytes[byte]) << (byte * 8U);
    return value;
}

void append_u64(std::vector<uint8_t>& output, uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) output.push_back(uint8_t(value >> shift));
}

struct StoredValue {
    std::string key;
    Value value;
};

bool is_ascii_space(char character) noexcept {
    return character == ' ' || character == '\t' || character == '\r' ||
        character == '\n' || character == '\v' || character == '\f';
}

std::string_view trim_ascii(std::string_view value) noexcept {
    while (!value.empty() && is_ascii_space(value.front())) value.remove_prefix(1);
    while (!value.empty() && is_ascii_space(value.back())) value.remove_suffix(1);
    return value;
}

char ascii_lower(char character) noexcept {
    if (character >= 'A' && character <= 'Z') return static_cast<char>(character + ('a' - 'A'));
    return character;
}

bool ascii_case_equal(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ascii_lower(left[index]) != ascii_lower(right[index])) return false;
    }
    return true;
}

std::string normalized_content_type(std::string_view value) {
    value = trim_ascii(value.substr(0, value.find(';')));
    std::string normalized(value);
    for (char& character : normalized) character = ascii_lower(character);
    return normalized;
}

std::string normalized_metadata(std::string_view value) {
    return std::string(trim_ascii(value));
}

void validate_content_url(const std::string& url) {
    if (url.empty() || url.size() > kMaxContentUrlBytes) {
        throw ScriptError("HTTP content URL must contain 1..8192 bytes");
    }
    if (!url.starts_with("http://") && !url.starts_with("https://")) {
        throw ScriptError("HTTP requests require an HTTP(S) URL");
    }
}

void validate_content_request(const ContentRequest& request) {
    if (request.method == ContentRequestMethod::get) {
        if (!request.body.empty()) throw ScriptError("HTTP GET content request body must be empty");
        return;
    }
    if (request.method == ContentRequestMethod::post) {
        if (request.body.size() > kMaxContentBodyBytes) {
            throw ScriptError("HTTP content request body is limited to 4096 bytes");
        }
        return;
    }
    throw ScriptError("unsupported HTTP content request method");
}

struct HttpWork {
    uint32_t request_id = 0;
    ContentRequest request;
    bool content = false;
    HttpResult result;
    enum class ResponseFault : uint8_t { none, too_large, allocation_failure } fault = ResponseFault::none;
};
struct QemuNetwork final {
    SemaphoreHandle_t ip_changed = nullptr;
    esp_eth_mac_t* mac = nullptr;
    esp_eth_phy_t* phy = nullptr;
    esp_eth_handle_t ethernet = nullptr;
    esp_netif_t* netif = nullptr;
    esp_eth_netif_glue_handle_t glue = nullptr;
    bool ethernet_started = false;
    bool ip_handler_registered = false;
    bool lost_ip_handler_registered = false;
    std::atomic<bool> online{false};

    QemuNetwork() {
        ip_changed = xSemaphoreCreateBinary();
        if (ip_changed == nullptr) throw std::runtime_error("cannot create DHCP completion semaphore");
    }

    ~QemuNetwork() {
        if (ip_handler_registered) {
            esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, &QemuNetwork::on_ip_event);
        }
        if (lost_ip_handler_registered) {
            esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_LOST_IP, &QemuNetwork::on_ip_event);
        }
        if (ethernet_started) check_esp_without_abort(esp_eth_stop(ethernet), "stopping OpenCores Ethernet");
        if (glue != nullptr) check_esp_without_abort(esp_eth_del_netif_glue(glue), "deleting Ethernet netif attachment");
        if (netif != nullptr) esp_netif_destroy(netif);
        if (ethernet != nullptr) check_esp_without_abort(esp_eth_driver_uninstall(ethernet), "uninstalling OpenCores Ethernet");
        if (mac != nullptr) check_esp_without_abort(mac->del(mac), "deleting OpenCores MAC");
        if (phy != nullptr) check_esp_without_abort(phy->del(phy), "deleting OpenCores PHY");
        if (ip_changed != nullptr) vSemaphoreDelete(ip_changed);
    }

    static void on_ip_event(void* argument, esp_event_base_t, int32_t event_id, void* event_data) {
        auto& network = *static_cast<QemuNetwork*>(argument);
        if (event_id == IP_EVENT_ETH_GOT_IP) {
            const auto& event = *static_cast<ip_event_got_ip_t*>(event_data);
            network.online.store(true, std::memory_order_release);
            ESP_LOGI(kLogTag, "QEMU OpenCores Ethernet address " IPSTR, IP2STR(&event.ip_info.ip));
        } else if (event_id == IP_EVENT_ETH_LOST_IP) {
            network.online.store(false, std::memory_order_release);
            ESP_LOGW(kLogTag, "QEMU OpenCores Ethernet address lost");
        }
        xSemaphoreGive(network.ip_changed);
    }

    void start() {
#if CONFIG_MB_QEMU_ETHERNET
        check_esp(esp_netif_init(), "initializing TCP/IP interfaces");
        check_esp(esp_event_loop_create_default(), "creating the default event loop");

        eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
        eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
        phy_config.autonego_timeout_ms = 100;
        mac = esp_eth_mac_new_openeth(&mac_config);
        if (mac == nullptr) throw std::runtime_error("creating OpenCores MAC failed");
        phy = esp_eth_phy_new_generic(&phy_config);
        if (phy == nullptr) throw std::runtime_error("creating OpenCores generic PHY failed");

        esp_eth_config_t ethernet_config = ETH_DEFAULT_CONFIG(mac, phy);
        check_esp(esp_eth_driver_install(&ethernet_config, &ethernet), "installing OpenCores Ethernet");

        esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
        netif = esp_netif_new(&netif_config);
        if (netif == nullptr) throw std::runtime_error("creating Ethernet network interface failed");
        glue = esp_eth_new_netif_glue(ethernet);
        if (glue == nullptr) throw std::runtime_error("attaching Ethernet to lwIP failed");
        check_esp(esp_netif_attach(netif, glue), "configuring Ethernet network interface");

        check_esp(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &QemuNetwork::on_ip_event, this),
            "registering DHCP completion handler");
        ip_handler_registered = true;
        check_esp(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, &QemuNetwork::on_ip_event, this),
            "registering DHCP loss handler");
        lost_ip_handler_registered = true;
        check_esp(esp_eth_start(ethernet), "starting OpenCores Ethernet");
        ethernet_started = true;

        const bool event_received = xSemaphoreTake(ip_changed, pdMS_TO_TICKS(kDhcpTimeoutMs)) == pdTRUE;
        if (!event_received || !online.load(std::memory_order_acquire)) {
            ESP_LOGW(kLogTag, "QEMU network offline: no DHCP lease within %" PRIu32 " ms; local UI remains available", kDhcpTimeoutMs);
        }
#else
        ESP_LOGW(kLogTag, "QEMU network offline: CONFIG_MB_QEMU_ETHERNET is disabled");
#endif
    }
};

esp_err_t metadata_allocation_failure(HttpWork& work) {
    work.fault = HttpWork::ResponseFault::allocation_failure;
    work.result.body.clear();
    return ESP_ERR_NO_MEM;
}

void clear_response_metadata(HttpResult& result) noexcept {
    result.content_type.clear();
    result.total_height.clear();
    result.image_width.clear();
    result.image_height.clear();
    result.image_y.clear();
}

esp_err_t receive_http_header(esp_http_client_event_t* event, HttpWork& work) {
    if (!work.content) return ESP_OK;
    if (event->header_key == nullptr) return ESP_ERR_INVALID_ARG;
    const std::string_view name(event->header_key);
    if (!ascii_case_equal(name, "Content-Type") &&
        !ascii_case_equal(name, "X-Microbrowser-Total-Height") &&
        !ascii_case_equal(name, "X-Microbrowser-Width") &&
        !ascii_case_equal(name, "X-Microbrowser-Height") &&
        !ascii_case_equal(name, "X-Microbrowser-Y")) {
        return ESP_OK;
    }
    if (event->header_value == nullptr) return ESP_ERR_INVALID_ARG;
    const std::string_view value(event->header_value);
    try {
        if (ascii_case_equal(name, "Content-Type")) {
            work.result.content_type = normalized_content_type(value);
        } else if (ascii_case_equal(name, "X-Microbrowser-Total-Height")) {
            work.result.total_height = normalized_metadata(value);
        } else if (ascii_case_equal(name, "X-Microbrowser-Width")) {
            work.result.image_width = normalized_metadata(value);
        } else if (ascii_case_equal(name, "X-Microbrowser-Height")) {
            work.result.image_height = normalized_metadata(value);
        } else {
            work.result.image_y = normalized_metadata(value);
        }
    } catch (const std::bad_alloc&) {
        return metadata_allocation_failure(work);
    }
    return ESP_OK;
}

esp_err_t receive_http_data(esp_http_client_event_t* event, HttpWork& work) {
    if (event->data_len < 0 || (event->data_len > 0 && event->data == nullptr)) return ESP_ERR_INVALID_ARG;
    const std::size_t length = std::size_t(event->data_len);
    if (length == 0) return ESP_OK;
    const std::size_t limit = work.content ? kMaxContentResponseBytes : kMaxScriptResponseBytes;
    if (length > limit - work.result.body.size()) {
        work.fault = HttpWork::ResponseFault::too_large;
        work.result.body.clear();
        return ESP_ERR_INVALID_SIZE;
    }
    try {
        work.result.body.append(static_cast<const char*>(event->data), length);
    } catch (const std::bad_alloc&) {
        return metadata_allocation_failure(work);
    }
    return ESP_OK;
}

esp_err_t receive_http_event(esp_http_client_event_t* event) {
    auto& work = *static_cast<HttpWork*>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_HEADER) return receive_http_header(event, work);
    if (event->event_id == HTTP_EVENT_ON_DATA) return receive_http_data(event, work);
    if (event->event_id == HTTP_EVENT_ON_STATUS_CODE && work.content) {
        clear_response_metadata(work.result);
    }
    return ESP_OK;
}

void capture_response_url(esp_http_client_handle_t client, HttpWork& work) {
    try {
        work.result.response_url.assign(kMaxContentUrlBytes + 1, '\0');
        const esp_err_t url_error = esp_http_client_get_url(
            client, work.result.response_url.data(), int(work.result.response_url.size()));
        if (url_error != ESP_OK) {
            work.result.response_url.clear();
            work.result.error = "HTTP response error: effective URL is unavailable";
            return;
        }
        work.result.response_url.resize(std::char_traits<char>::length(work.result.response_url.data()));
    } catch (const std::bad_alloc&) {
        work.result.response_url.clear();
        metadata_allocation_failure(work);
    }
}

void perform_http_request(HttpWork& work) {
    work.result = {};
    work.result.id = work.request_id;
    work.fault = HttpWork::ResponseFault::none;

    try {
        esp_http_client_config_t config = {};
        config.url = work.request.url.c_str();
        config.method = work.content && work.request.method == ContentRequestMethod::post
            ? HTTP_METHOD_POST : HTTP_METHOD_GET;
        config.timeout_ms = kHttpTimeoutMs;
        config.max_redirection_count = work.content ? kMaxContentRedirects : kMaxScriptRedirects;
        config.event_handler = &receive_http_event;
        config.user_data = &work;
        config.buffer_size = 2048;
        config.buffer_size_tx = 4096;
        config.crt_bundle_attach = esp_crt_bundle_attach;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == nullptr) {
            work.result.error = "HTTP client initialization failed";
            return;
        }
        struct Cleanup {
            esp_http_client_handle_t client;
            ~Cleanup() { check_esp_without_abort(esp_http_client_cleanup(client), "closing HTTP client"); }
        } cleanup{client};

        if (work.content && work.request.method == ContentRequestMethod::post) {
            check_esp(esp_http_client_set_header(client, "Content-Type", "application/json"),
                "setting HTTP content type");
            check_esp(esp_http_client_set_post_field(client, work.request.body.data(), int(work.request.body.size())),
                "setting HTTP content body");
        }
        const esp_err_t error = esp_http_client_perform(client);
        work.result.status = esp_http_client_get_status_code(client);
        if (work.content && error == ESP_OK && work.fault == HttpWork::ResponseFault::none) {
            capture_response_url(client, work);
        }
        if (work.fault == HttpWork::ResponseFault::too_large) {
            work.result.error = work.content ? "HTTP response exceeds the 2097152-byte limit"
                                             : "HTTP response exceeds the 65536-byte limit";
        } else if (work.fault == HttpWork::ResponseFault::allocation_failure) {
            work.result.error = "HTTP response allocation failed";
        } else if (error != ESP_OK) {
            work.result.error = std::string("HTTP request failed: ") + esp_err_to_name(error);
        }
    } catch (const std::exception& error) {
        work.result.error = std::string("HTTP worker exception: ") + error.what();
    }
}

} // namespace

struct EspIdfPlatform::State final {
    std::array<StoredValue, kStorageRecords> storage{};
    nvs_handle_t nvs{};
    std::array<HttpWork, kRequestLimit> slots{};
    QueueHandle_t free_slots = nullptr;
    QueueHandle_t request_queue = nullptr;
    QueueHandle_t completion_queue = nullptr;
    QueueHandle_t content_completion_queue = nullptr;
    SemaphoreHandle_t worker_done = nullptr;
    TaskHandle_t worker = nullptr;
    std::unique_ptr<QemuNetwork> network;
    bool shut_down = false;

    State() {
        ESP_LOGI(kLogTag, "initialize_storage");
        try {
            initialize_storage();
            ESP_LOGI(kLogTag, "initialize_worker");
            initialize_worker();
        } catch (...) {
            shutdown();
            throw;
        }
    }

    ~State() { shutdown(); }

    std::array<char, 4> storage_slot_name(std::size_t index) {
        std::array<char, 4> name{};
        std::snprintf(name.data(), name.size(), "r%02zu", index);
        return name;
    }

    StoredValue decode_storage(std::span<const uint8_t> bytes, std::size_t index) const {
        const auto malformed = [index](std::string_view reason) {
            return std::runtime_error("malformed NVS record r" + std::to_string(index) + ": " + std::string(reason));
        };
        if (bytes.size() < kStorageHeaderBytes) throw malformed("truncated header");
        if (bytes[0] != kStorageFormatVersion) throw malformed("unsupported version");
        const std::size_t key_length = read_u16(bytes, 2);
        const std::size_t value_length = read_u16(bytes, 4);
        if (key_length == 0 || key_length > kMaxStorageKeyBytes) throw malformed("invalid key length");
        if (bytes.size() != kStorageHeaderBytes + key_length + value_length) throw malformed("wrong record length");

        StoredValue decoded;
        decoded.key.assign(reinterpret_cast<const char*>(bytes.data() + kStorageHeaderBytes), key_length);
        std::span<const uint8_t> encoded = bytes.subspan(kStorageHeaderBytes + key_length);
        switch (Value::Type(bytes[1])) {
        case Value::Type::number:
            if (value_length != sizeof(float)) throw malformed("number must contain four bytes");
            {
                uint32_t bits = 0;
                for (std::size_t byte = 0; byte < encoded.size(); ++byte) bits |= uint32_t(encoded[byte]) << (8U * byte);
                const float number = std::bit_cast<float>(bits);
                if (!std::isfinite(number)) throw malformed("non-finite number");
                decoded.value = Value::from_number(number);
            }
            break;
        case Value::Type::integer:
            if (value_length != sizeof(int64_t)) throw malformed("integer must contain eight bytes");
            decoded.value = Value::from_integer(std::bit_cast<int64_t>(read_u64(encoded)));
            break;
        case Value::Type::bytes:
            if (value_length > kMaxStorageBinaryBytes) throw malformed("binary value is too long");
            decoded.value = Value::from_bytes(ByteBuffer(encoded.begin(), encoded.end()));
            break;
        case Value::Type::boolean:
            if (value_length != 1 || encoded[0] > 1) throw malformed("invalid Boolean value");
            decoded.value = Value::from_bool(encoded[0] != 0);
            break;
        case Value::Type::string:
            if (value_length > kMaxStorageStringBytes) throw malformed("string value is too long");
            decoded.value = Value::from_string(std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size()));
            break;
        default:
            throw malformed("unknown value type");
        }
        return decoded;
    }

    std::vector<uint8_t> encode_storage(const std::string& name, const Value& value) const {
        if (name.empty() || name.size() > kMaxStorageKeyBytes) {
            throw ScriptError("storage key must contain 1..128 bytes");
        }
        std::size_t encoded_size = 0;
        switch (value.type) {
        case Value::Type::number:
            if (!std::isfinite(value.number)) throw ScriptError("cannot store a non-finite number");
            encoded_size = sizeof(float);
            break;
        case Value::Type::boolean:
            encoded_size = 1;
            break;
        case Value::Type::integer:
            encoded_size = sizeof(int64_t);
            break;
        case Value::Type::bytes:
            if (!value.bytes) throw ScriptError("cannot store a null byte buffer");
            if (value.bytes->size() > kMaxStorageBinaryBytes) throw ScriptError("stored binary values are limited to 4096 bytes");
            encoded_size = value.bytes->size();
            break;
        case Value::Type::string:
            if (value.string.size() > kMaxStorageStringBytes) throw ScriptError("stored strings are limited to 4096 bytes");
            encoded_size = value.string.size();
            break;
        case Value::Type::handle:
            throw ScriptError("cannot store a device handle");
        case Value::Type::void_value:
            throw ScriptError("cannot store a void value");
        default:
            throw ScriptError("void values cannot be stored");
        }

        std::vector<uint8_t> bytes;
        bytes.reserve(kStorageHeaderBytes + name.size() + encoded_size);
        bytes.push_back(kStorageFormatVersion);
        bytes.push_back(uint8_t(value.type));
        append_u16(bytes, uint16_t(name.size()));
        append_u16(bytes, uint16_t(encoded_size));
        bytes.insert(bytes.end(), name.begin(), name.end());
        if (value.type == Value::Type::number) {
            append_u32(bytes, std::bit_cast<uint32_t>(value.number));
        } else if (value.type == Value::Type::integer) {
            append_u64(bytes, std::bit_cast<uint64_t>(value.integer));
        } else if (value.type == Value::Type::bytes) {
            bytes.insert(bytes.end(), value.bytes->begin(), value.bytes->end());
        } else if (value.type == Value::Type::boolean) {
            bytes.push_back(value.boolean ? 1 : 0);
        } else {
            bytes.insert(bytes.end(), value.string.begin(), value.string.end());
        }
        return bytes;
    }

    void initialize_storage() {
        esp_err_t error = nvs_flash_init();
        if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            check_esp(nvs_flash_erase(), "erasing incompatible or full NVS partition");
            error = nvs_flash_init();
        }
        check_esp(error, "initializing NVS flash");
        check_esp(nvs_open(kStorageNamespace, NVS_READWRITE, &nvs), "opening Microbrowser NVS namespace");

        for (std::size_t index = 0; index < storage.size(); ++index) {
            const auto slot = storage_slot_name(index);
            std::size_t length = 0;
            error = nvs_get_blob(nvs, slot.data(), nullptr, &length);
            if (error == ESP_ERR_NVS_NOT_FOUND) continue;
            check_esp(error, "reading NVS record");
            if (length > kMaxStorageRecordBytes) {
                throw std::runtime_error("malformed NVS record " + std::string(slot.data()) + ": excessive length");
            }
            if (length == 0) throw std::runtime_error("malformed NVS record " + std::string(slot.data()) + ": empty blob");
            std::vector<uint8_t> bytes(length);
            check_esp(nvs_get_blob(nvs, slot.data(), bytes.data(), &length), "loading NVS record");
            storage[index] = decode_storage(bytes, index);
            for (std::size_t previous = 0; previous < index; ++previous) {
                if (!storage[previous].key.empty() && storage[previous].key == storage[index].key) {
                    throw std::runtime_error("malformed NVS storage: duplicate application key");
                }
            }
        }
    }

    static void worker_entry(void* argument) {
        auto& state = *static_cast<State*>(argument);
        for (;;) {
            HttpWork* work = nullptr;
            if (xQueueReceive(state.request_queue, &work, portMAX_DELAY) != pdTRUE) continue;
            if (work == nullptr) break;
            perform_http_request(*work);
            const QueueHandle_t completions = work->content ? state.content_completion_queue : state.completion_queue;
            if (xQueueSend(completions, &work, 0) != pdTRUE) {
                ESP_LOGE(kLogTag, "HTTP completion queue overflow for request %" PRIu32, work->request_id);
                std::abort();
            }
        }
        xSemaphoreGive(state.worker_done);
        vTaskDelete(nullptr);
    }

    void initialize_worker() {
        free_slots = xQueueCreate(kRequestLimit, sizeof(HttpWork*));
        request_queue = xQueueCreate(kRequestLimit, sizeof(HttpWork*));
        completion_queue = xQueueCreate(kRequestLimit, sizeof(HttpWork*));
        content_completion_queue = xQueueCreate(kRequestLimit, sizeof(HttpWork*));
        worker_done = xSemaphoreCreateBinary();
        if (free_slots == nullptr || request_queue == nullptr || completion_queue == nullptr ||
            content_completion_queue == nullptr || worker_done == nullptr) {
            throw std::runtime_error("allocating bounded HTTP queues failed");
        }
        for (HttpWork& slot : slots) {
            HttpWork* pointer = &slot;
            if (xQueueSend(free_slots, &pointer, 0) != pdTRUE) throw std::runtime_error("initializing HTTP slot pool failed");
        }
        const BaseType_t created = xTaskCreate(&State::worker_entry, "mb_http", kHttpWorkerStackBytes, this,
            kHttpWorkerPriority, &worker);
        if (created != pdPASS) throw std::runtime_error("creating HTTP worker task failed");
    }

    void shutdown() {
        if (shut_down) return;
        if (worker != nullptr) {
            HttpWork* stop = nullptr;
            if (xQueueSend(request_queue, &stop, pdMS_TO_TICKS(kShutdownTimeoutMs)) != pdTRUE ||
                xSemaphoreTake(worker_done, pdMS_TO_TICKS(kShutdownTimeoutMs)) != pdTRUE) {
                ESP_LOGE(kLogTag, "HTTP worker did not stop within %" PRIu32 " ms", kShutdownTimeoutMs);
                std::abort();
            }
            worker = nullptr;
        }
        network.reset();
        if (free_slots != nullptr) vQueueDelete(free_slots);
        if (request_queue != nullptr) vQueueDelete(request_queue);
        if (completion_queue != nullptr) vQueueDelete(completion_queue);
        if (content_completion_queue != nullptr) vQueueDelete(content_completion_queue);
        if (worker_done != nullptr) vSemaphoreDelete(worker_done);
        free_slots = nullptr;
        request_queue = nullptr;
        completion_queue = nullptr;
        content_completion_queue = nullptr;
        worker_done = nullptr;
        if (nvs != 0) {
            nvs_close(nvs);
            nvs = 0;
        }
        shut_down = true;
    }

    void start_request(uint32_t id, std::string url) {
        if (url.empty() || url.size() > kMaxScriptUrlBytes) {
            throw ScriptError("HTTP URL must contain 1..2047 bytes");
        }
        enqueue_request(id, ContentRequest{std::move(url), ContentRequestMethod::get, std::string{}}, false);
    }

    void start_content(uint32_t id, ContentRequest request) {
        validate_content_url(request.url);
        validate_content_request(request);
        enqueue_request(id, std::move(request), true);
    }

    void enqueue_request(uint32_t id, ContentRequest request, bool content) {
        HttpWork* work = nullptr;
        if (xQueueReceive(free_slots, &work, 0) != pdTRUE) {
            throw ScriptError("HTTP request queue is full (8 outstanding requests)");
        }
        work->request_id = id;
        work->request = std::move(request);
        work->content = content;
        work->result = {};
        work->result.id = id;
        if (xQueueSend(request_queue, &work, 0) != pdTRUE) {
            ESP_LOGE(kLogTag, "HTTP input queue overflow for request %" PRIu32, id);
            if (xQueueSend(free_slots, &work, 0) != pdTRUE) std::abort();
            throw ScriptError("HTTP input queue is full");
        }
    }

    bool poll_request(HttpResult& result) {
        return poll_completion(result, completion_queue);
    }

    bool poll_content(HttpResult& result) {
        return poll_completion(result, content_completion_queue);
    }

    bool poll_completion(HttpResult& result, QueueHandle_t completions) {
        HttpWork* work = nullptr;
        if (xQueueReceive(completions, &work, 0) != pdTRUE) return false;
        result = std::move(work->result);
        if (xQueueSend(free_slots, &work, 0) != pdTRUE) {
            ESP_LOGE(kLogTag, "HTTP slot pool overflow while consuming request %" PRIu32, result.id);
            std::abort();
        }
        return true;
    }

    std::optional<Value> load_value(const std::string& name) const {
        const auto found = std::find_if(storage.begin(), storage.end(), [&](const StoredValue& record) {
            return !record.key.empty() && record.key == name;
        });
        if (found == storage.end()) return std::nullopt;
        return found->value;
    }

    void save_value(const std::string& name, const Value& value) {
        std::vector<uint8_t> encoded = encode_storage(name, value);
        auto found = std::find_if(storage.begin(), storage.end(), [&](const StoredValue& record) {
            return !record.key.empty() && record.key == name;
        });
        if (found == storage.end()) {
            found = std::find_if(storage.begin(), storage.end(), [](const StoredValue& record) {
                return record.key.empty();
            });
        }
        if (found == storage.end()) throw ScriptError("all 64 persistent storage records are in use");
        const std::size_t index = std::size_t(found - storage.begin());
        const auto slot = storage_slot_name(index);
        check_esp(nvs_set_blob(nvs, slot.data(), encoded.data(), encoded.size()), "writing NVS value");
        check_esp(nvs_commit(nvs), "committing NVS value");
        *found = StoredValue{name, value};
    }
};

EspIdfPlatform::EspIdfPlatform() : m_state(std::make_unique<State>()) {}
EspIdfPlatform::~EspIdfPlatform() = default;

void EspIdfPlatform::start_network() {
    if (!m_state->network) {
        m_state->network = std::make_unique<QemuNetwork>();
        m_state->network->start();
    }
}

bool EspIdfPlatform::network_online() const {
    return m_state->network != nullptr && m_state->network->online.load(std::memory_order_acquire);
}

uint64_t EspIdfPlatform::now_ms() const {
    return uint64_t(esp_timer_get_time() / 1000);
}

void EspIdfPlatform::start_request(uint32_t id, std::string url) {
    m_state->start_request(id, std::move(url));
}

bool EspIdfPlatform::poll_request(HttpResult& result) {
    return m_state->poll_request(result);
}

void EspIdfPlatform::start_content(uint32_t id, ContentRequest request) {
    m_state->start_content(id, std::move(request));
}

bool EspIdfPlatform::poll_content(HttpResult& result) {
    return m_state->poll_content(result);
}

std::optional<Value> EspIdfPlatform::load_value(const std::string& name) const {
    return m_state->load_value(name);
}

void EspIdfPlatform::save_value(const std::string& name, const Value& value) {
    m_state->save_value(name, value);
}

void EspIdfPlatform::log(std::string_view message) {
    if (message.size() > std::size_t(std::numeric_limits<int>::max())) {
        ESP_LOGE(kLogTag, "application diagnostic exceeds logger limits");
        return;
    }
    ESP_LOGI(kLogTag, "%.*s", int(message.size()), message.data());
}

} // namespace mb
