#include "desktop.h"

#include "microbrowser/vm.h"

#include <array>
#include <atomic>
#include <bit>
#include <climits>
#include <cstdlib>
#include <curl/curl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace mb {
namespace {

constexpr std::size_t max_outstanding_requests = 8;
constexpr std::size_t max_response_bytes = 64 * 1024;
constexpr long connect_timeout_seconds = 3;
constexpr long total_timeout_seconds = 10;
constexpr long maximum_redirects = 5;

static_assert(sizeof(float) == sizeof(uint32_t) && std::numeric_limits<float>::is_iec559,
              "binary32 floats are required for persistent number values");

std::filesystem::path default_state_path() {
    if (const char* override_path = std::getenv("MB_STATE_FILE")) {
        if (*override_path == '\0') throw ScriptError("MB_STATE_FILE must not be empty");
        return std::filesystem::path(override_path);
    }
    return ".microbrowser-state";
}

std::mutex& curl_global_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::size_t& curl_global_users() {
    static std::size_t users = 0;
    return users;
}

void acquire_curl() {
    std::lock_guard lock(curl_global_mutex());
    std::size_t& users = curl_global_users();
    if (users == 0) {
        const CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (code != CURLE_OK) {
            throw std::runtime_error(std::string("libcurl initialization failed: ") +
                                     curl_easy_strerror(code));
        }
    }
    ++users;
}

void release_curl() noexcept {
    std::lock_guard lock(curl_global_mutex());
    std::size_t& users = curl_global_users();
    if (users == 0) return;
    --users;
    if (users == 0) curl_global_cleanup();
}

struct CurlHandleDeleter {
    void operator()(CURL* handle) const noexcept {
        if (handle != nullptr) curl_easy_cleanup(handle);
    }
};
using CurlHandle = std::unique_ptr<CURL, CurlHandleDeleter>;

struct CurlOptionError {
    CURLcode code;
};

template <typename T>
void set_curl_option(CURL* handle, CURLoption option, T value) {
    const CURLcode code = curl_easy_setopt(handle, option, value);
    if (code != CURLE_OK) throw CurlOptionError{code};
}

struct ResponseBuffer {
    enum class Failure : uint8_t { none, too_large, allocation };

    std::string body;
    Failure failure = Failure::none;
};

size_t response_write_callback(char* data, size_t size, size_t count, void* opaque) noexcept {
    ResponseBuffer& response = *static_cast<ResponseBuffer*>(opaque);
    if (count == 0) return 0;
    if (size != 0 && count > std::numeric_limits<size_t>::max() / size) {
        response.failure = ResponseBuffer::Failure::too_large;
        return 0;
    }
    const size_t bytes = size * count;
    if (bytes > max_response_bytes - response.body.size()) {
        response.failure = ResponseBuffer::Failure::too_large;
        return 0;
    }
    try {
        response.body.append(data, bytes);
    } catch (...) {
        response.failure = ResponseBuffer::Failure::allocation;
        return 0;
    }
    return bytes;
}

void configure_request(CURL* handle, const std::string& url, ResponseBuffer& response) {
    curl_easy_reset(handle);
    set_curl_option(handle, CURLOPT_URL, url.c_str());
    set_curl_option(handle, CURLOPT_HTTPGET, 1L);
    set_curl_option(handle, CURLOPT_FOLLOWLOCATION, 1L);
    set_curl_option(handle, CURLOPT_MAXREDIRS, maximum_redirects);
    set_curl_option(handle, CURLOPT_TIMEOUT, total_timeout_seconds);
    set_curl_option(handle, CURLOPT_CONNECTTIMEOUT, connect_timeout_seconds);
    set_curl_option(handle, CURLOPT_NOSIGNAL, 1L);
    set_curl_option(handle, CURLOPT_SSL_VERIFYPEER, 1L);
    set_curl_option(handle, CURLOPT_SSL_VERIFYHOST, 2L);
    set_curl_option(handle, CURLOPT_MAXFILESIZE_LARGE,
                    static_cast<curl_off_t>(max_response_bytes));
    set_curl_option(handle, CURLOPT_USERAGENT, "microbrowser-desktop/1");
#if LIBCURL_VERSION_NUM >= 0x075500
    set_curl_option(handle, CURLOPT_PROTOCOLS_STR, "http,https");
    set_curl_option(handle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    set_curl_option(handle, CURLOPT_PROTOCOLS,
                    static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    set_curl_option(handle, CURLOPT_REDIRPROTOCOLS,
                    static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    set_curl_option(handle, CURLOPT_WRITEFUNCTION, response_write_callback);
    set_curl_option(handle, CURLOPT_WRITEDATA, &response);
}

HttpResult perform_request(CURL* handle, uint32_t id, const std::string& url) {
    HttpResult result;
    result.id = id;
    if (handle == nullptr) {
        result.error = "HTTP setup error: could not create a libcurl handle";
        return result;
    }

    ResponseBuffer response;
    CURLcode code = CURLE_OK;
    try {
        configure_request(handle, url, response);
        code = curl_easy_perform(handle);
    } catch (const CurlOptionError& error) {
        result.error = std::string("HTTP setup error: ") + curl_easy_strerror(error.code);
        return result;
    }

    long status = 0;
    const CURLcode status_code = curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    if (status_code == CURLE_OK && status >= 0 && status <= INT_MAX) {
        result.status = static_cast<int>(status);
    } else if (code == CURLE_OK) {
        result.error = status_code == CURLE_OK
            ? "HTTP response error: status code is outside the supported range"
            : std::string("HTTP response error: ") + curl_easy_strerror(status_code);
    }

    if (response.failure == ResponseBuffer::Failure::too_large) {
        result.error = "HTTP response error: body exceeds the 65536-byte limit";
    } else if (response.failure == ResponseBuffer::Failure::allocation) {
        result.error = "HTTP response error: could not allocate the bounded body buffer";
    } else if (code != CURLE_OK) {
        result.error = std::string("HTTP transport error: ") + curl_easy_strerror(code);
    }
    if (!result.error.empty()) {
        response.body.clear();
        return result;
    }
    result.body = std::move(response.body);
    return result;
}

using Records = std::map<std::string, Value, std::less<>>;

[[noreturn]] void throw_state_error(const std::filesystem::path& path,
                                    std::string_view detail) {
    throw ScriptError("state storage '" + path.string() + "': " + std::string(detail));
}

int hex_digit(unsigned char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

std::string decode_hex(std::string_view encoded) {
    if (encoded.size() % 2 != 0) throw std::invalid_argument("hex text has an odd length");
    std::string decoded;
    decoded.reserve(encoded.size() / 2);
    for (std::size_t offset = 0; offset < encoded.size(); offset += 2) {
        const int high = hex_digit(static_cast<unsigned char>(encoded[offset]));
        const int low = hex_digit(static_cast<unsigned char>(encoded[offset + 1]));
        if (high < 0 || low < 0) throw std::invalid_argument("invalid hex text");
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    return decoded;
}

uint32_t decode_number(std::string_view encoded) {
    if (encoded.size() != 8) throw std::invalid_argument("number encoding must contain eight hex digits");
    uint32_t bits = 0;
    for (const char character : encoded) {
        const int digit = hex_digit(static_cast<unsigned char>(character));
        if (digit < 0) throw std::invalid_argument("invalid number encoding");
        bits = (bits << 4U) | static_cast<uint32_t>(digit);
    }
    return bits;
}

Value parse_record(std::string_view line) {
    if (line.size() < 3 || line[1] != ':') {
        throw std::invalid_argument("record is missing its type separator");
    }
    const std::size_t value_separator = line.find(':', 2);
    if (value_separator == std::string_view::npos) {
        throw std::invalid_argument("record is missing its value separator");
    }
    const std::string_view encoded_value = line.substr(value_separator + 1);
    switch (line[0]) {
    case 'n':
        return Value::from_number(std::bit_cast<float>(decode_number(encoded_value)));
    case 'b':
        if (encoded_value == "0") return Value::from_bool(false);
        if (encoded_value == "1") return Value::from_bool(true);
        throw std::invalid_argument("boolean encoding must be 0 or 1");
    case 's':
        return Value::from_string(decode_hex(encoded_value));
    default:
        throw std::invalid_argument("unknown persistent value type");
    }
}

std::string_view record_key(std::string_view line) {
    const std::size_t value_separator = line.find(':', 2);
    return line.substr(2, value_separator - 2);
}

std::optional<Records> read_records(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (!error && !exists) return std::nullopt;
        throw_state_error(path, "cannot open the state file for reading");
    }

    Records records;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        try {
            Value value = parse_record(line);
            std::string key = decode_hex(record_key(line));
            if (!records.emplace(std::move(key), std::move(value)).second) {
                throw std::invalid_argument("duplicate persistent key");
            }
        } catch (const std::invalid_argument& error) {
            throw_state_error(path, "line " + std::to_string(line_number) + ": " + error.what());
        }
    }
    if (input.bad() || !input.eof()) throw_state_error(path, "failed while reading the state file");
    return records;
}

struct TemporaryFile {
    std::filesystem::path path;
    bool renamed = false;

    ~TemporaryFile() {
        if (!renamed) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    }
};

TemporaryFile temporary_path_for(const std::filesystem::path& destination) {
    static std::atomic<uint64_t> sequence = 0;
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path path = destination;
    path += ".tmp." + std::to_string(static_cast<unsigned long long>(timestamp)) + "." +
        std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    return TemporaryFile{std::move(path), false};
}

void write_hex(std::ostream& output, std::string_view bytes) {
    constexpr std::array<char, 16> digits{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };
    for (const unsigned char byte : bytes) {
        const std::array<char, 2> encoded{digits[byte >> 4U], digits[byte & 0x0fU]};
        output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    }
}

void write_number(std::ostream& output, float number) {
    constexpr std::array<char, 16> digits{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };
    uint32_t bits = std::bit_cast<uint32_t>(number);
    std::array<char, 8> encoded{};
    for (std::size_t index = encoded.size(); index > 0; --index) {
        encoded[index - 1] = digits[bits & 0x0fU];
        bits >>= 4U;
    }
    output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
}

void write_records(const std::filesystem::path& destination, const Records& records) {
    TemporaryFile temporary = temporary_path_for(destination);
    {
        std::ofstream output(temporary.path, std::ios::binary | std::ios::trunc);
        if (!output) throw_state_error(destination, "cannot open a temporary state file");
        for (const auto& [name, value] : records) {
            switch (value.type) {
            case Value::Type::number:
                output.put('n');
                break;
            case Value::Type::boolean:
                output.put('b');
                break;
            case Value::Type::string:
                output.put('s');
                break;
            case Value::Type::void_value:
                throw_state_error(destination, "cannot persist a void value");
            }
            output.put(':');
            write_hex(output, name);
            output.put(':');
            if (value.type == Value::Type::number) write_number(output, value.number);
            if (value.type == Value::Type::boolean) output.put(value.boolean ? '1' : '0');
            if (value.type == Value::Type::string) write_hex(output, value.string);
            output.put('\n');
        }
        output.flush();
        if (!output) throw_state_error(destination, "failed while writing a temporary state file");
        output.close();
        if (!output) throw_state_error(destination, "failed to close a temporary state file");
    }

    std::error_code error;
    std::filesystem::rename(temporary.path, destination, error);
    if (error) throw_state_error(destination, "atomic rename failed: " + error.message());
    temporary.renamed = true;
}

} // namespace

DesktopPlatform::DesktopPlatform() : m_state_path(default_state_path()) {
    acquire_curl();
    try {
        m_worker = std::thread([this] { worker_loop(); });
    } catch (...) {
        release_curl();
        throw;
    }
}

DesktopPlatform::~DesktopPlatform() {
    {
        std::lock_guard lock(m_request_mutex);
        m_stopping = true;
        m_pending_requests.clear();
    }
    m_request_ready.notify_one();
    if (m_worker.joinable()) m_worker.join();
    release_curl();
}

uint64_t DesktopPlatform::now_ms() const {
    const auto elapsed = std::chrono::steady_clock::now() - m_epoch;
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

void DesktopPlatform::start_request(uint32_t id, std::string url) {
    {
        std::lock_guard lock(m_request_mutex);
        if (m_stopping) throw ScriptError("cannot queue HTTP work while the platform is stopping");
        if (m_outstanding_requests >= max_outstanding_requests) {
            throw ScriptError("HTTP request queue is full (8 outstanding requests)");
        }
        m_pending_requests.push_back(Request{id, std::move(url)});
        ++m_outstanding_requests;
    }
    m_request_ready.notify_one();
}

bool DesktopPlatform::poll_request(HttpResult& result) {
    std::lock_guard lock(m_request_mutex);
    if (m_completed_requests.empty()) return false;
    result = std::move(m_completed_requests.front());
    m_completed_requests.pop_front();
    --m_outstanding_requests;
    return true;
}

void DesktopPlatform::worker_loop() {
    CurlHandle handle(curl_easy_init());
    for (;;) {
        Request request;
        {
            std::unique_lock lock(m_request_mutex);
            m_request_ready.wait(lock, [this] { return m_stopping || !m_pending_requests.empty(); });
            if (m_stopping) break;
            request = std::move(m_pending_requests.front());
            m_pending_requests.pop_front();
        }

        HttpResult result = perform_request(handle.get(), request.id, request.url);
        if (!result.error.empty()) {
            log("HTTP request " + std::to_string(request.id) + " failed: " + result.error);
        } else if (result.status != 0 && !result.succeeded()) {
            log("HTTP request " + std::to_string(request.id) + " returned status " +
                std::to_string(result.status));
        }

        {
            std::lock_guard lock(m_request_mutex);
            if (m_stopping) break;
            m_completed_requests.push_back(std::move(result));
        }
    }
}

std::optional<Value> DesktopPlatform::load_value(const std::string& name) const {
    std::lock_guard lock(m_storage_mutex);
    std::optional<Records> records = read_records(m_state_path);
    if (!records) return std::nullopt;
    const auto found = records->find(name);
    if (found == records->end()) return std::nullopt;
    return found->second;
}

void DesktopPlatform::save_value(const std::string& name, const Value& value) {
    if (value.type == Value::Type::void_value) {
        throw ScriptError("cannot persist a void value for key '" + name + "'");
    }
    std::lock_guard lock(m_storage_mutex);
    Records records = read_records(m_state_path).value_or(Records{});
    auto found = records.find(name);
    if (found == records.end()) records.emplace(name, value);
    else found->second = value;
    write_records(m_state_path, records);
}

void DesktopPlatform::log(std::string_view message) {
    std::lock_guard lock(m_log_mutex);
    std::cerr << "[microbrowser] ";
    for (const unsigned char character : message) {
        if (character == '\n' || character == '\r' || character == '\t') std::cerr.put(' ');
        else if (character < 0x20U || character == 0x7fU) std::cerr.put('?');
        else std::cerr.put(static_cast<char>(character));
    }
    std::cerr.put('\n');
    std::cerr.flush();
}

} // namespace mb
