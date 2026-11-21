#include "platform/zephyr_http.h"

#include "microbrowser/vm.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <system_error>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

LOG_MODULE_REGISTER(mb_zephyr_http, LOG_LEVEL_INF);

#if defined(CONFIG_MB_HTTP_ENABLED) && CONFIG_MB_HTTP_ENABLED
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/http/parser.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/socket_types.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/fdtable.h>
#endif

#if defined(CONFIG_MB_HTTP_ENABLED) && CONFIG_MB_HTTP_ENABLED

namespace mb {
namespace {
constexpr std::size_t kSlotCount = 8;
constexpr std::size_t kMaximumContentBodyBytes = 4096;
constexpr std::size_t kMaximumUrlBytes = 8192;
constexpr std::size_t kMaximumPackageBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumHostBytes = 253;
constexpr std::size_t kMaximumHeaderNameBytes = 64;
constexpr std::size_t kMaximumHeaderValueBytes = 512;
constexpr std::size_t kWorkerStackBytes = 12U * 1024U;
constexpr int kWorkerPriority = 7;
constexpr int kCancellationPollMs = 100;

enum class SlotState : uint8_t { free, queued, active, completed };
enum class ResponseFault : uint8_t { none, too_large, allocation, metadata };

struct ParsedUrl {
    bool tls = false;
    std::string host;
    std::string port;
    std::string request_host;
    std::string target;
};

void validate_url_characters(std::string_view url) {
    if (url.empty() || url.size() > kMaximumUrlBytes) {
        throw ScriptError("HTTP URL must contain 1..8192 bytes");
    }
    for (const unsigned char byte : url) {
        if (byte <= 0x20U || byte >= 0x7fU) throw ScriptError("HTTP URL contains an invalid character");
    }
    if (url.find('#') != std::string_view::npos) throw ScriptError("HTTP URL fragments are not supported");
}

uint16_t parse_port(std::string_view port, bool tls) {
    if (port.empty()) return tls ? 443 : 80;
    if (port.size() > 5) throw ScriptError("HTTP URL port must contain 1..5 decimal digits");
    uint32_t value = 0;
    const auto converted = std::from_chars(port.data(), port.data() + port.size(), value);
    if (converted.ec != std::errc{} || converted.ptr != port.data() + port.size() || value == 0 || value > 65535) {
        throw ScriptError("HTTP URL port must be a decimal integer in 1..65535");
    }
    return uint16_t(value);
}


ParsedUrl parse_http_url(std::string_view url, bool allow_plain_http, int tls_ca_security_tag) {
    validate_url_characters(url);
    ParsedUrl parsed;
    const bool tls = url.starts_with("https://");
    if (!tls && !url.starts_with("http://")) throw ScriptError("HTTP requests require an HTTP(S) URL");
    if (!tls && !allow_plain_http) throw ScriptError("plain HTTP is disabled by the board owner");
    if (tls && tls_ca_security_tag < 0) throw ScriptError("HTTPS requires an owner-provisioned TLS CA security tag");
    parsed.tls = tls;
    const std::size_t scheme_bytes = tls ? 8 : 7;
    const std::string_view remainder = url.substr(scheme_bytes);
    const std::size_t path_begin = remainder.find_first_of("/?");
    const std::string_view authority = remainder.substr(0, path_begin);
    if (authority.empty()) throw ScriptError("HTTP URL host is empty");

    if (authority.find('@') != std::string_view::npos) {
        throw ScriptError("HTTP URL user information is not supported");
    }
    std::string_view host;
    std::string_view port_text;
    std::string header_host;
    bool explicit_port = false;
    if (authority.front() == '[') {
        const std::size_t closing = authority.find(']');
        if (closing == std::string_view::npos || closing == 1 || closing + 1 > authority.size()) {
            throw ScriptError("HTTP URL IPv6 authority is invalid");
        }
        host = authority.substr(1, closing - 1);
        header_host = std::string(authority.substr(0, closing + 1));
        if (closing + 1 != authority.size()) {
            if (authority[closing + 1] != ':') throw ScriptError("HTTP URL IPv6 authority is invalid");
            explicit_port = true;
            port_text = authority.substr(closing + 2);
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            if (authority.find(':') != colon) throw ScriptError("IPv6 HTTP literals must be bracketed");
            explicit_port = true;
            host = authority.substr(0, colon);
            port_text = authority.substr(colon + 1);
        } else {
            host = authority;
        }
        header_host = std::string(host);
    }
    if (host.empty() || host.size() > kMaximumHostBytes) throw ScriptError("HTTP URL host must contain 1..253 bytes");
    if (explicit_port && port_text.empty()) throw ScriptError("HTTP URL port must not be empty");
    const uint16_t port_number = parse_port(port_text, tls);
    parsed.host.assign(host);
    parsed.port = std::to_string(port_number);
    parsed.request_host = std::move(header_host);
    parsed.target = path_begin == std::string_view::npos ? "/" : std::string(remainder.substr(path_begin));
    if (parsed.target.front() == '?') parsed.target.insert(parsed.target.begin(), '/');
    return parsed;
}

struct HttpWork {
    SlotState slot_state = SlotState::free;
    ZephyrRequestKind kind = ZephyrRequestKind::script;
    uint32_t request_id = 0;
    ContentRequest request;
    ParsedUrl parsed_url;
    HttpResult result;
    std::vector<uint8_t> receive_buffer;
    std::size_t response_limit_bytes = 0;
    void* state = nullptr;
    std::atomic<bool> cancellation_requested{false};
    int socket = -1;
    ResponseFault fault = ResponseFault::none;
    std::string header_name;
    std::string header_value;
    bool header_in_value = false;

    void reset() {
        slot_state = SlotState::free;
        state = nullptr;
        request_id = 0;
        request = {};
        parsed_url = {};
        result = {};
        receive_buffer.clear();
        response_limit_bytes = 0;
        cancellation_requested.store(false, std::memory_order_release);
        socket = -1;
        fault = ResponseFault::none;
        header_name.clear();
        header_value.clear();
        header_in_value = false;
    }
};

bool ascii_space(char byte) noexcept {
    return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n' || byte == '\v' || byte == '\f';
}

std::string_view trim_ascii(std::string_view value) noexcept {
    while (!value.empty() && ascii_space(value.front())) value.remove_prefix(1);
    while (!value.empty() && ascii_space(value.back())) value.remove_suffix(1);
    return value;
}

char ascii_lower(char byte) noexcept {
    return byte >= 'A' && byte <= 'Z' ? char(byte + ('a' - 'A')) : byte;
}

bool ascii_equal(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ascii_lower(left[index]) != ascii_lower(right[index])) return false;
    }
    return true;
}

std::string normalized_content_type(std::string_view value) {
    value = trim_ascii(value.substr(0, value.find(';')));
    std::string normalized(value);
    for (char& byte : normalized) byte = ascii_lower(byte);
    return normalized;
}

std::string error_message(std::string_view operation, int error) {
    return std::string(operation) + " failed with errno " + std::to_string(error < 0 ? -error : error);
}

} // namespace

struct ZephyrHttp::State final {
    explicit State(ZephyrHttpConfig config_) : config(config_) {
        if (!config.enabled) return;
        validate_config();
        k_mutex_init(&mutex);
        k_sem_init(&work_ready, 0, kSlotCount + 2);
        k_sem_init(&network_changed, 0, 1);
        k_msgq_init(&input_queue, input_storage.data(), sizeof(uint8_t), kSlotCount);
        for (std::size_t index = 0; index < completion_queues.size(); ++index) {
            k_msgq_init(&completion_queues[index], completion_storage[index].data(), sizeof(uint8_t), kSlotCount);
        }
        net_mgmt_init_event_callback(&wifi_callback, &State::wifi_event,
                                     NET_EVENT_WIFI_CONNECT_RESULT |
                                         NET_EVENT_WIFI_DISCONNECT_RESULT |
                                         NET_EVENT_WIFI_DISCONNECT_COMPLETE);
        net_mgmt_add_event_callback(&wifi_callback);
        net_mgmt_init_event_callback(&ipv4_callback, &State::ipv4_event,
                                     NET_EVENT_IPV4_ADDR_ADD | NET_EVENT_IPV4_ADDR_DEL |
                                         NET_EVENT_IPV4_DHCP_BOUND | NET_EVENT_IPV4_DHCP_STOP);
        net_mgmt_add_event_callback(&ipv4_callback);
        callbacks_registered = true;
        worker_id = k_thread_create(&worker, worker_stack, K_THREAD_STACK_SIZEOF(worker_stack),
                                    &State::worker_entry, this, nullptr, nullptr,
                                    kWorkerPriority, 0, K_NO_WAIT);
        worker_started = true;
        (void)k_thread_name_set(worker_id, "mb_http");
    }

    ~State() { shutdown(); }

    void validate_config() const {
        if (config.receive_buffer_bytes < 256 || config.receive_buffer_bytes > 65536) {
            throw std::invalid_argument("Zephyr HTTP receive buffer must contain 256..65536 bytes");
        }
        if (config.script_response_limit_bytes == 0 || config.content_response_limit_bytes == 0) {
            throw std::invalid_argument("Zephyr HTTP response limits must be positive");
        }
        for (const uint32_t milliseconds : {config.wifi_connect_timeout_ms,
                                            config.socket_connect_timeout_ms,
                                            config.request_timeout_ms,
                                            config.shutdown_timeout_ms}) {
            if (milliseconds == 0 || milliseconds > uint32_t(std::numeric_limits<int32_t>::max())) {
                throw std::invalid_argument("Zephyr HTTP timeouts must fit a signed 32-bit millisecond value");
            }
        }

        const uint64_t teardown_work_ms = uint64_t(CONFIG_NET_SOCKETS_DNS_TIMEOUT) +
            config.socket_connect_timeout_ms + 2ULL * config.request_timeout_ms + 1000;
        if (config.shutdown_timeout_ms < teardown_work_ms) {
            throw std::invalid_argument(
                "Zephyr HTTP shutdown timeout must cover DNS, connect, request, and socket-close bounds");
        }
    }

    static void wifi_event(net_mgmt_event_callback* callback, uint64_t event, net_if*) {
        auto& state = *CONTAINER_OF(callback, State, wifi_callback);
        if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
            state.connect_completed.store(true, std::memory_order_release);
#ifdef CONFIG_NET_MGMT_EVENT_INFO
            const auto* status = static_cast<const wifi_status*>(callback->info);
            state.connect_failed.store(status == nullptr || status->status != 0, std::memory_order_release);
#endif
        } else {
            state.online.store(false, std::memory_order_release);
        }
        k_sem_give(&state.network_changed);
    }

    static void ipv4_event(net_mgmt_event_callback* callback, uint64_t event, net_if*) {
        auto& state = *CONTAINER_OF(callback, State, ipv4_callback);
        if (event == NET_EVENT_IPV4_ADDR_ADD || event == NET_EVENT_IPV4_DHCP_BOUND) {
            state.online.store(true, std::memory_order_release);
        } else {
            state.online.store(false, std::memory_order_release);
        }
        k_sem_give(&state.network_changed);
    }

    static void worker_entry(void* argument, void*, void*) {
        static_cast<State*>(argument)->worker_loop();
    }

    void start_network_once() {
        if (!config.enabled) {
            LOG_WRN("network start requested while HTTP support is disabled");
            return;
        }
        k_mutex_lock(&mutex, K_FOREVER);
        const bool duplicate = network_requested || stopping;
        if (!stopping) network_requested = true;
        k_mutex_unlock(&mutex);
        if (!duplicate) k_sem_give(&work_ready);
    }

    void start(ZephyrRequestKind kind, uint32_t id, ContentRequest request,
               std::optional<std::size_t> package_limit = std::nullopt) {
        if (!config.enabled) throw ScriptError("HTTP transport is disabled for this board profile");
        if (id == 0) throw ScriptError("HTTP request identifier zero is reserved");
        validate_request(kind, request);
        const std::size_t limit = package_limit.value_or(
            kind == ZephyrRequestKind::script ? config.script_response_limit_bytes
                                             : config.content_response_limit_bytes);
        if (limit == 0 || limit > kMaximumPackageBytes) throw ScriptError("HTTP response limit is outside its bounds");
        ParsedUrl parsed = parse_http_url(request.url, config.allow_plain_http, config.tls_ca_security_tag);
        start_network_once();

        k_mutex_lock(&mutex, K_FOREVER);
        if (stopping) {
            k_mutex_unlock(&mutex);
            throw ScriptError("cannot submit HTTP work during platform teardown");
        }
        HttpWork* available = nullptr;
        for (HttpWork& candidate : slots) {
            if (candidate.slot_state != SlotState::free && candidate.kind == kind && candidate.request_id == id) {
                k_mutex_unlock(&mutex);
                throw ScriptError("duplicate HTTP request identifier");
            }
            if (available == nullptr && candidate.slot_state == SlotState::free) available = &candidate;
        }
        if (available == nullptr) {
            k_mutex_unlock(&mutex);
            throw ScriptError("all 8 bounded HTTP jobs are in use");
        }
        try {
            available->state = this;
            available->kind = kind;
            available->request_id = id;
            available->request = std::move(request);
            available->parsed_url = std::move(parsed);
            available->result.id = id;
            available->response_limit_bytes = limit;
            available->receive_buffer.resize(config.receive_buffer_bytes);
        } catch (...) {
            available->reset();
            k_mutex_unlock(&mutex);
            throw;
        }
        const uint8_t index = uint8_t(available - slots.data());
        available->slot_state = SlotState::queued;
        const int queued = k_msgq_put(&input_queue, &index, K_NO_WAIT);
        k_mutex_unlock(&mutex);
        if (queued != 0) {
            k_mutex_lock(&mutex, K_FOREVER);
            available->reset();
            k_mutex_unlock(&mutex);
            throw std::runtime_error(error_message("HTTP input queue submission", queued));
        }
        k_sem_give(&work_ready);
    }

    void validate_request(ZephyrRequestKind kind, const ContentRequest& request) const {
        if (kind != ZephyrRequestKind::content) {
            if (request.method != ContentRequestMethod::get || !request.body.empty()) {
                throw ScriptError("script and package HTTP downloads require a bodyless GET");
            }
            return;
        }
        if (request.method == ContentRequestMethod::get) {
            if (!request.body.empty()) throw ScriptError("HTTP GET content body must be empty");
        } else if (request.method == ContentRequestMethod::post) {
            if (request.body.size() > kMaximumContentBodyBytes) {
                throw ScriptError("HTTP POST content body is limited to 4096 bytes");
            }
        } else {
            throw ScriptError("unsupported HTTP content method");
        }
    }

    void connect_wifi() {
        k_mutex_lock(&mutex, K_FOREVER);
        const bool should_connect = network_requested && !network_attempted && !stopping;
        network_attempted = network_attempted || should_connect;
        k_mutex_unlock(&mutex);
        if (!should_connect) return;
        int result = conn_mgr_all_if_up(true);
        if (result == 0) result = conn_mgr_all_if_connect(true);
        if (result != 0) {
            connect_failed.store(true, std::memory_order_release);
            connect_completed.store(true, std::memory_order_release);
            LOG_ERR("owner-provisioned Wi-Fi connection failed: %d; automatic retry is disabled", result);
            return;
        }
        const int64_t deadline = k_uptime_get() + config.wifi_connect_timeout_ms;
        while (!online.load(std::memory_order_acquire) && !stopping) {
            if (connect_completed.load(std::memory_order_acquire) &&
                connect_failed.load(std::memory_order_acquire)) {
                LOG_ERR("Wi-Fi association failed; automatic retry is disabled");
                return;
            }
            const int64_t remaining = deadline - k_uptime_get();
            if (remaining <= 0) {
                LOG_ERR("Wi-Fi/DHCP timed out after %u ms; automatic retry is disabled",
                        static_cast<unsigned>(config.wifi_connect_timeout_ms));
                return;
            }
            (void)k_sem_take(&network_changed, K_MSEC(int32_t(std::min<int64_t>(remaining, 1000))));
        }
        // This is only a failed connection notification, not a retry indicator.
        // Discard it so subsequent explicit operations see the current state.
    }

    void worker_loop() {
        for (;;) {
            (void)k_sem_take(&work_ready, K_FOREVER);
            connect_wifi();
            if (stopping) break;
            uint8_t index = 0;
            if (k_msgq_get(&input_queue, &index, K_NO_WAIT) != 0) continue;
            if (index >= kSlotCount) std::abort();
            process_job(index);
        }
    }

    void process_job(uint8_t index) {
        HttpWork& work = slots[index];
        k_mutex_lock(&mutex, K_FOREVER);
        if (work.slot_state != SlotState::queued) {
            k_mutex_unlock(&mutex);
            return;
        }
        work.slot_state = SlotState::active;
        k_mutex_unlock(&mutex);

        work.result = {};
        work.result.id = work.request_id;
        work.fault = ResponseFault::none;
        try {
            if (work.cancellation_requested.load(std::memory_order_acquire) || stopping) {
                work.result.error = "HTTP request cancelled";
            } else if (!network_attempted || !online.load(std::memory_order_acquire)) {
                work.result.error = connect_failed.load(std::memory_order_acquire)
                    ? "network unavailable: owner Wi-Fi connection failed"
                    : "network unavailable: no IPv4/DHCP lease";
            } else {
                perform_request(work);
            }
        } catch (const std::bad_alloc&) {
            work.result = {};
            work.result.id = work.request_id;
            work.result.error = "HTTP allocation failed";
        } catch (const std::exception& error) {
            work.result.error = std::string("HTTP worker error: ") + error.what();
        }
        if (work.cancellation_requested.load(std::memory_order_acquire) && work.fault == ResponseFault::none) {
            work.result = {};
            work.result.id = work.request_id;
            work.result.error = "HTTP request cancelled";
        }
        if (work.result.error.empty() && work.result.status == 0) {
            work.result.error = "HTTP server returned no status";
        }
        k_mutex_lock(&mutex, K_FOREVER);
        work.slot_state = SlotState::completed;
        work.result.id = work.request_id;
        const int completed = k_msgq_put(&completion_queues[size_t(work.kind)], &index, K_NO_WAIT);
        k_mutex_unlock(&mutex);
        if (completed != 0) {
            LOG_ERR("HTTP completion queue overflow: %d", completed);
            std::abort();
        }
    }

    int response_callback(http_response& response, HttpWork& work) noexcept {
        work.result.status = response.http_status_code;
        if (response.cl_present && response.content_length > work.response_limit_bytes) {
            work.fault = ResponseFault::too_large;
            return -EMSGSIZE;
        }
        return work.cancellation_requested.load(std::memory_order_acquire) ? -ECANCELED : 0;
    }

    int body_callback(const char* data, size_t length, HttpWork& work) noexcept {
        try {
            if (work.cancellation_requested.load(std::memory_order_acquire)) return -ECANCELED;
            if (length == 0) return 0;
            if (data == nullptr) return -EINVAL;
            if (length > work.response_limit_bytes - work.result.body.size()) {
                work.fault = ResponseFault::too_large;
                return -EMSGSIZE;
            }
            work.result.body.append(data, length);
            return 0;
        } catch (const std::bad_alloc&) {
            work.fault = ResponseFault::allocation;
            return -ENOMEM;
        } catch (...) {
            return -EINVAL;
        }
    }

    int header_field_callback(http_parser& parser, const char* data, size_t length, HttpWork& work) noexcept {
        try {
            if (work.header_in_value) commit_header(work);
            if (length > kMaximumHeaderNameBytes - std::min(work.header_name.size(), kMaximumHeaderNameBytes)) {
                work.fault = ResponseFault::metadata;
                return -E2BIG;
            }
            work.header_name.append(data, length);
            (void)parser;
            return 0;
        } catch (const std::bad_alloc&) {
            work.fault = ResponseFault::allocation;
            return -ENOMEM;
        }
    }

    int header_value_callback(const char* data, size_t length, HttpWork& work) noexcept {
        try {
            work.header_in_value = true;
            if (length > kMaximumHeaderValueBytes - std::min(work.header_value.size(), kMaximumHeaderValueBytes)) {
                work.fault = ResponseFault::metadata;
                return -E2BIG;
            }
            work.header_value.append(data, length);
            return 0;
        } catch (const std::bad_alloc&) {
            work.fault = ResponseFault::allocation;
            return -ENOMEM;
        }
    }

    void commit_header(HttpWork& work) {
        const std::string_view name = work.header_name;
        const std::string_view value = trim_ascii(work.header_value);
        if (ascii_equal(name, "Content-Type")) work.result.content_type = normalized_content_type(value);
        else if (ascii_equal(name, "X-Microbrowser-Total-Height")) work.result.total_height.assign(value);
        else if (ascii_equal(name, "X-Microbrowser-Width")) work.result.image_width.assign(value);
        else if (ascii_equal(name, "X-Microbrowser-Height")) work.result.image_height.assign(value);
        else if (ascii_equal(name, "X-Microbrowser-Y")) work.result.image_y.assign(value);
        work.header_name.clear();
        work.header_value.clear();
        work.header_in_value = false;
    }

    static int on_response(http_response* response, http_final_call, void* argument) {
        auto& work = *static_cast<HttpWork*>(argument);
        auto& state = *static_cast<State*>(work.state);
        return state.response_callback(*response, work);
    }

    int open_socket(HttpWork& work) {
        const ParsedUrl& target = work.parsed_url;
        zsock_addrinfo hints{};
        hints.ai_family = NET_AF_UNSPEC;
        hints.ai_socktype = NET_SOCK_STREAM;
        hints.ai_protocol = NET_IPPROTO_TCP;
        zsock_addrinfo* addresses = nullptr;
        const int resolved = zsock_getaddrinfo(target.host.c_str(), target.port.c_str(), &hints, &addresses);
        if (resolved != 0 || addresses == nullptr) {
            if (addresses != nullptr) zsock_freeaddrinfo(addresses);
            return resolved == 0 ? -EHOSTUNREACH : -EIO;
        }
        struct AddressCleanup {
            zsock_addrinfo* value;
            ~AddressCleanup() { zsock_freeaddrinfo(value); }
        } cleanup{addresses};
        const int socket = zsock_socket(addresses->ai_family, addresses->ai_socktype,
                                        target.tls ? int(NET_IPPROTO_TLS_1_2) : int(NET_IPPROTO_TCP));
        if (socket < 0) return -errno;
        k_mutex_lock(&mutex, K_FOREVER);
        work.socket = socket;
        const bool cancelled = work.cancellation_requested.load(std::memory_order_acquire);
        if (cancelled) {
            (void)zsock_close(socket);
            work.socket = -1;
        }
        k_mutex_unlock(&mutex);
        if (cancelled) return -ECANCELED;
        int error = configure_socket(socket, target);
        if (error != 0) return error;
        return connect_socket(work, socket, *addresses);
    }

    int configure_socket(int socket, const ParsedUrl& target) const {
        zsock_timeval timeout{};
        timeout.tv_sec = config.request_timeout_ms / 1000;
        timeout.tv_usec = (config.request_timeout_ms % 1000) * 1000;
        if (zsock_setsockopt(socket, ZSOCK_SOL_SOCKET, ZSOCK_SO_RCVTIMEO,
                             &timeout, sizeof(timeout)) != 0) return -errno;
        if (zsock_setsockopt(socket, ZSOCK_SOL_SOCKET, ZSOCK_SO_SNDTIMEO,
                             &timeout, sizeof(timeout)) != 0) return -errno;
        if (!target.tls) return 0;
        const sec_tag_t security_tags[] = {sec_tag_t(config.tls_ca_security_tag)};
        if (zsock_setsockopt(socket, ZSOCK_SOL_TLS, ZSOCK_TLS_SEC_TAG_LIST,
                             security_tags, sizeof(security_tags)) != 0) return -errno;
        const int verification = ZSOCK_TLS_PEER_VERIFY_REQUIRED;
        if (zsock_setsockopt(socket, ZSOCK_SOL_TLS, ZSOCK_TLS_PEER_VERIFY,
                             &verification, sizeof(verification)) != 0) return -errno;
        if (zsock_setsockopt(socket, ZSOCK_SOL_TLS, ZSOCK_TLS_HOSTNAME,
                             target.host.c_str(), target.host.size() + 1) != 0) return -errno;
        return 0;
    }

    int connect_socket(HttpWork& work, int socket, const zsock_addrinfo& address) {
        const int flags = zsock_fcntl(socket, ZVFS_F_GETFL, 0);
        if (flags < 0 || zsock_fcntl(socket, ZVFS_F_SETFL, flags | ZVFS_O_NONBLOCK) != 0) return -errno;
        int result = zsock_connect(socket, address.ai_addr, address.ai_addrlen);
        if (result != 0 && errno != EINPROGRESS && errno != EWOULDBLOCK) return -errno;
        const int64_t deadline = k_uptime_get() + config.socket_connect_timeout_ms;
        if (result != 0) {
            for (;;) {
                if (work.cancellation_requested.load(std::memory_order_acquire)) return -ECANCELED;
                const int64_t remaining = deadline - k_uptime_get();
                if (remaining <= 0) return -ETIMEDOUT;
                zsock_pollfd descriptor{};
                descriptor.fd = socket;
                descriptor.events = ZSOCK_POLLOUT;
                result = zsock_poll(&descriptor, 1, int(std::min<int64_t>(remaining, kCancellationPollMs)));
                if (result < 0) return -errno;
                if (result > 0) {
                    if (work.cancellation_requested.load(std::memory_order_acquire)) return -ECANCELED;
                    if ((descriptor.revents & ZSOCK_POLLNVAL) != 0) return -EBADF;
                    int socket_error = 0;
                    net_socklen_t length = sizeof(socket_error);
                    if (zsock_getsockopt(socket, ZSOCK_SOL_SOCKET, ZSOCK_SO_ERROR,
                                         &socket_error, &length) != 0) return -errno;
                    if (socket_error != 0) return -socket_error;
                    if ((descriptor.revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP)) != 0) return -ECONNREFUSED;
                    if ((descriptor.revents & ZSOCK_POLLOUT) != 0) break;
                }
            }
        }
        const int connected_flags = zsock_fcntl(socket, ZVFS_F_GETFL, 0);
        if (connected_flags < 0 ||
            zsock_fcntl(socket, ZVFS_F_SETFL, connected_flags & ~ZVFS_O_NONBLOCK) != 0) return -errno;
        return 0;
    }

    void close_worker_socket(HttpWork& work, int socket) noexcept {
        k_mutex_lock(&mutex, K_FOREVER);
        if (work.socket == socket) {
            (void)zsock_close(socket);
            work.socket = -1;
        }
        k_mutex_unlock(&mutex);
    }

    void perform_request(HttpWork& work) {
        const int socket = open_socket(work);
        if (socket < 0) {
            if (work.socket >= 0) close_worker_socket(work, work.socket);
            work.result.error = work.cancellation_requested.load(std::memory_order_acquire)
                ? "HTTP request cancelled" : error_message("HTTP connection", socket);
            return;
        }
        struct SocketCleanup {
            State& state;
            HttpWork& work;
            int socket;
            ~SocketCleanup() { state.close_worker_socket(work, socket); }
        } cleanup{*this, work, socket};

        http_request request{};
        request.method = work.kind == ZephyrRequestKind::content &&
                                 work.request.method == ContentRequestMethod::post
            ? HTTP_POST : HTTP_GET;
        request.response = &State::on_response;
        request.http_cb = nullptr;
        request.recv_buf = work.receive_buffer.data();
        request.recv_buf_len = work.receive_buffer.size();
        request.url = work.parsed_url.target.c_str();
        request.protocol = "HTTP/1.1";
        request.host = work.parsed_url.request_host.c_str();
        request.port = work.parsed_url.port.c_str();
        const char* optional_headers[] = {"Connection: close\r\n", nullptr};
        request.optional_headers = optional_headers;
        if (request.method == HTTP_POST) {
            request.content_type_value = "application/json";
            request.payload = work.request.body.data();
            request.payload_len = work.request.body.size();
        }
        request.internal.parser.data = this;
        http_parser_settings parser_settings;
        http_parser_settings_init(&parser_settings);
        active_work = &work;
        parser_settings.on_body = [](http_parser* parser, const char* data, size_t length) {
            auto& state = *static_cast<State*>(parser->data);
            return state.body_callback(data, length, *state.active_work);
        };
        parser_settings.on_header_field = [](http_parser* parser, const char* data, size_t length) {
            auto& state = *static_cast<State*>(parser->data);
            return state.header_field_callback(*parser, data, length, *state.active_work);
        };
        parser_settings.on_header_value = [](http_parser* parser, const char* data, size_t length) {
            auto& state = *static_cast<State*>(parser->data);
            return state.header_value_callback(data, length, *state.active_work);
        };
        parser_settings.on_headers_complete = [](http_parser* parser) {
            auto& state = *static_cast<State*>(parser->data);
            try {
                state.commit_header(*state.active_work);
                return 0;
            } catch (const std::bad_alloc&) {
                state.active_work->fault = ResponseFault::allocation;
                return -ENOMEM;
            }
        };
        request.http_cb = &parser_settings;
        const int transferred = http_client_req(socket, &request, int32_t(config.request_timeout_ms), &work);
        active_work = nullptr;
        if (work.fault == ResponseFault::too_large) work.result.error = "HTTP response exceeds its configured byte limit";
        else if (work.fault == ResponseFault::allocation) work.result.error = "HTTP response allocation failed";
        else if (work.fault == ResponseFault::metadata) work.result.error = "HTTP response metadata exceeds its bounds";
        else if (transferred < 0 && work.result.error.empty()) work.result.error = error_message("HTTP request", transferred);
        if (work.result.status != 0) work.result.response_url = work.request.url;
        if (work.fault != ResponseFault::none) work.result.body.clear();
    }

    bool poll(ZephyrRequestKind kind, HttpResult& result) {
        if (!config.enabled) return false;
        uint8_t index = 0;
        if (k_msgq_get(&completion_queues[size_t(kind)], &index, K_NO_WAIT) != 0) return false;
        if (index >= kSlotCount) std::abort();
        k_mutex_lock(&mutex, K_FOREVER);
        HttpWork& work = slots[index];
        if (work.slot_state != SlotState::completed || work.kind != kind) std::abort();
        result = std::move(work.result);
        work.reset();
        k_mutex_unlock(&mutex);
        return true;
    }

    bool cancel(ZephyrRequestKind kind, uint32_t id) {
        if (!config.enabled) return false;
        k_mutex_lock(&mutex, K_FOREVER);
        for (HttpWork& work : slots) {
            if (work.kind != kind || work.request_id != id ||
                (work.slot_state != SlotState::queued && work.slot_state != SlotState::active)) continue;
            work.cancellation_requested.store(true, std::memory_order_release);
            k_mutex_unlock(&mutex);
            return true;
        }
        k_mutex_unlock(&mutex);
        return false;
    }

    void shutdown() noexcept {
        if (!config.enabled || shutting_down) return;
        shutting_down = true;
        if (worker_started) {
            k_mutex_lock(&mutex, K_FOREVER);
            stopping = true;
            for (HttpWork& work : slots) {
                work.cancellation_requested.store(true, std::memory_order_release);
            }
            k_mutex_unlock(&mutex);
            k_sem_give(&work_ready);
            k_sem_give(&network_changed);
            const int joined = k_thread_join(worker_id, K_MSEC(config.shutdown_timeout_ms));
            if (joined != 0) {
                LOG_ERR("HTTP worker failed to stop within %u ms", static_cast<unsigned>(config.shutdown_timeout_ms));
                std::abort();
            }
            worker_started = false;
        }
        if (callbacks_registered) {
            net_mgmt_del_event_callback(&wifi_callback);
            net_mgmt_del_event_callback(&ipv4_callback);
            callbacks_registered = false;
        }
    }

    ZephyrHttpConfig config;
    std::array<HttpWork, kSlotCount> slots{};
    k_mutex mutex{};
    k_sem work_ready{};
    k_sem network_changed{};
    k_msgq input_queue{};
    std::array<char, kSlotCount * sizeof(uint8_t)> input_storage{};
    std::array<k_msgq, 3> completion_queues{};
    std::array<std::array<char, kSlotCount * sizeof(uint8_t)>, 3> completion_storage{};
    k_thread worker{};
    k_tid_t worker_id = nullptr;
    K_KERNEL_STACK_MEMBER(worker_stack, kWorkerStackBytes);
    net_mgmt_event_callback wifi_callback{};
    net_mgmt_event_callback ipv4_callback{};
    HttpWork* active_work = nullptr;
    std::atomic<bool> online{false};
    std::atomic<bool> connect_completed{false};
    std::atomic<bool> connect_failed{false};
    bool network_requested = false;
    bool network_attempted = false;
    std::atomic<bool> stopping{false};
    bool shutting_down = false;
    bool worker_started = false;
    bool callbacks_registered = false;
};

ZephyrHttp::ZephyrHttp(ZephyrHttpConfig config) : m_state(std::make_unique<State>(config)) {}
ZephyrHttp::~ZephyrHttp() = default;
void ZephyrHttp::start_network() { m_state->start_network_once(); }
bool ZephyrHttp::network_online() const { return m_state->online.load(std::memory_order_acquire); }
void ZephyrHttp::start(ZephyrRequestKind kind, uint32_t id, ContentRequest request) {
    m_state->start(kind, id, std::move(request));
}
void ZephyrHttp::start_package(uint32_t id, std::string url, std::size_t maximum_bytes) {
    m_state->start(ZephyrRequestKind::package, id,
                   ContentRequest{std::move(url), ContentRequestMethod::get, {}}, maximum_bytes);
}
bool ZephyrHttp::poll(ZephyrRequestKind kind, HttpResult& result) { return m_state->poll(kind, result); }
bool ZephyrHttp::cancel(ZephyrRequestKind kind, uint32_t id) { return m_state->cancel(kind, id); }
void ZephyrHttp::shutdown() noexcept { m_state->shutdown(); }

} // namespace mb

#else

namespace mb {

struct ZephyrHttp::State final {
    explicit State(ZephyrHttpConfig config_) : config(config_) {}
    void start_network_once() { LOG_WRN("HTTP networking is unavailable on this board profile"); }
    void start(ZephyrRequestKind, uint32_t, ContentRequest, std::optional<std::size_t> = std::nullopt) {
        throw ScriptError("HTTP transport is not compiled for this board profile");
    }
    bool poll(ZephyrRequestKind, HttpResult&) { return false; }
    bool cancel(ZephyrRequestKind, uint32_t) { return false; }
    void shutdown() noexcept {}
    ZephyrHttpConfig config;
};

ZephyrHttp::ZephyrHttp(ZephyrHttpConfig config) : m_state(std::make_unique<State>(config)) {}
ZephyrHttp::~ZephyrHttp() = default;
void ZephyrHttp::start_network() { m_state->start_network_once(); }
bool ZephyrHttp::network_online() const { return false; }
void ZephyrHttp::start(ZephyrRequestKind kind, uint32_t id, ContentRequest request) {
    m_state->start(kind, id, std::move(request));
}
void ZephyrHttp::start_package(uint32_t id, std::string url, std::size_t maximum_bytes) {
    m_state->start(ZephyrRequestKind::package, id,
                   ContentRequest{std::move(url), ContentRequestMethod::get, {}}, maximum_bytes);
}
bool ZephyrHttp::poll(ZephyrRequestKind kind, HttpResult& result) { return m_state->poll(kind, result); }
bool ZephyrHttp::cancel(ZephyrRequestKind kind, uint32_t id) { return m_state->cancel(kind, id); }
void ZephyrHttp::shutdown() noexcept { m_state->shutdown(); }

} // namespace mb

#endif
