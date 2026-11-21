#include "platform/zephyr_platform.h"

#include "microbrowser/vm.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

LOG_MODULE_REGISTER(mb_zephyr_platform, LOG_LEVEL_INF);

namespace mb {
namespace {

constexpr uint8_t kStorageVersion = 1;
constexpr std::size_t kStorageHeaderBytes = 8;
constexpr std::size_t kMaximumStorageKeyBytes = 128;
constexpr std::size_t kMaximumStorageStringBytes = 4096;
constexpr std::size_t kMaximumStorageByteBufferBytes = 4096;
constexpr std::size_t kMaximumStorageRecordBytes =
    kStorageHeaderBytes + kMaximumStorageKeyBytes + kMaximumStorageStringBytes;
constexpr std::size_t kMaximumLogBytes = 256;
constexpr std::size_t kRecordedLogEntries = 32;
static_assert(sizeof(double) == sizeof(uint64_t) && std::numeric_limits<double>::is_iec559,
              "binary64 is required for persistent numeric values");

struct StoredRecord {
    std::string key;
    Value value;
};

uint16_t read_u16(std::span<const uint8_t> bytes, std::size_t offset) {
    return uint16_t(bytes[offset]) | (uint16_t(bytes[offset + 1]) << 8U);
}

uint32_t read_u32(std::span<const uint8_t> bytes, std::size_t offset) {
    uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index) {
        value |= uint32_t(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

uint64_t read_u64(std::span<const uint8_t> bytes, std::size_t offset) {
    uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) {
        value |= uint64_t(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

void append_u16(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(uint8_t(value));
    bytes.push_back(uint8_t(value >> 8U));
}

void append_u32(std::vector<uint8_t>& bytes, uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) bytes.push_back(uint8_t(value >> (index * 8U)));
}

void append_u64(std::vector<uint8_t>& bytes, uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) bytes.push_back(uint8_t(value >> (index * 8U)));
}

void validate_storage_key(const std::string& name) {
    if (name.empty() || name.size() > kMaximumStorageKeyBytes) {
        throw ScriptError("Zephyr storage key must contain 1..128 bytes");
    }
}

std::string settings_key(const std::string& name) {
    uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : name) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::array<char, 24> key{};
    const int length = std::snprintf(key.data(), key.size(), "mb/v/%016llx",
                                     static_cast<unsigned long long>(hash));
    if (length != 21) throw std::runtime_error("could not encode Zephyr settings key");
    return std::string(key.data(), std::size_t(length));
}

[[noreturn]] void throw_settings_error(std::string_view operation, int error) {
    throw std::runtime_error("Zephyr settings " + std::string(operation) +
                             " failed with errno " + std::to_string(-error));
}

std::optional<StoredRecord> read_record(const std::string& name) {
    validate_storage_key(name);
    const std::string key = settings_key(name);
    const ssize_t encoded_size = settings_get_val_len(key.c_str());
    if (encoded_size < 0) throw_settings_error("length query", int(encoded_size));
    if (encoded_size == 0) return std::nullopt;
    if (std::size_t(encoded_size) < kStorageHeaderBytes ||
        std::size_t(encoded_size) > kMaximumStorageRecordBytes) {
        throw std::runtime_error("persisted Zephyr value has an invalid record size");
    }

    std::vector<uint8_t> encoded(encoded_size);
    const ssize_t actual = settings_load_one(key.c_str(), encoded.data(), encoded.size());
    if (actual < 0) throw_settings_error("read", int(actual));
    if (actual != encoded_size) throw std::runtime_error("persisted Zephyr value was truncated during loading");

    const std::span<const uint8_t> bytes(encoded);
    if (bytes[0] != kStorageVersion) throw std::runtime_error("persisted Zephyr value has an unsupported version");
    const std::size_t key_size = read_u16(bytes, 2);
    const std::size_t payload_size = read_u32(bytes, 4);
    if (key_size == 0 || key_size > kMaximumStorageKeyBytes ||
        bytes.size() != kStorageHeaderBytes + key_size + payload_size) {
        throw std::runtime_error("persisted Zephyr value has inconsistent field lengths");
    }
    std::string stored_key(reinterpret_cast<const char*>(bytes.data() + kStorageHeaderBytes), key_size);
    if (stored_key != name) {
        throw std::runtime_error("persisted Zephyr storage key collides with another application key");
    }
    const auto payload = bytes.subspan(kStorageHeaderBytes + key_size);
    const auto malformed = [](std::string_view detail) {
        return std::runtime_error("malformed Zephyr persistent value: " + std::string(detail));
    };

    StoredRecord record;
    record.key = std::move(stored_key);
    switch (Value::Type(bytes[1])) {
    case Value::Type::number: {
        if (payload.size() != sizeof(double)) throw malformed("number payload must contain eight bytes");
        const double number = std::bit_cast<double>(read_u64(payload, 0));
        const float narrowed = static_cast<float>(number);
        if (!std::isfinite(number) || !std::isfinite(narrowed) || static_cast<double>(narrowed) != number) {
            throw malformed("number is not finite and exactly representable by Value");
        }
        record.value = Value::from_number(narrowed);
        break;
    }
    case Value::Type::boolean:
        if (payload.size() != 1 || payload[0] > 1) throw malformed("Boolean payload must be zero or one");
        record.value = Value::from_bool(payload[0] != 0);
        break;
    case Value::Type::integer:
        if (payload.size() != sizeof(int64_t)) throw malformed("integer payload must contain eight bytes");
        record.value = Value::from_integer(std::bit_cast<int64_t>(read_u64(payload, 0)));
        break;
    case Value::Type::string:
        if (payload.size() > kMaximumStorageStringBytes) throw malformed("string exceeds its size cap");
        record.value = Value::from_string(
            std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
        break;
    case Value::Type::bytes:
        if (payload.size() > kMaximumStorageByteBufferBytes) throw malformed("byte buffer exceeds its size cap");
        record.value = Value::from_bytes(ByteBuffer(payload.begin(), payload.end()));
        break;
    case Value::Type::handle:
    case Value::Type::void_value:
        throw malformed("handles and void values are not persistent");
    default:
        throw malformed("unknown value type identifier");
    }
    return record;
}

std::vector<uint8_t> encode_record(const std::string& name, const Value& value) {
    validate_storage_key(name);
    std::size_t payload_size = 0;
    switch (value.type) {
    case Value::Type::number:
        if (!std::isfinite(value.number)) throw ScriptError("cannot persist a non-finite number");
        payload_size = sizeof(double);
        break;
    case Value::Type::boolean:
        payload_size = 1;
        break;
    case Value::Type::integer:
        payload_size = sizeof(int64_t);
        break;
    case Value::Type::string:
        if (value.string.size() > kMaximumStorageStringBytes) {
            throw ScriptError("persisted strings are limited to 4096 bytes");
        }
        payload_size = value.string.size();
        break;
    case Value::Type::bytes:
        if (!value.bytes) throw ScriptError("cannot persist a null byte buffer");
        if (value.bytes->size() > kMaximumStorageByteBufferBytes) {
            throw ScriptError("persisted byte buffers are limited to 4096 bytes");
        }
        payload_size = value.bytes->size();
        break;
    case Value::Type::handle:
        throw ScriptError("device handles cannot be persisted");
    case Value::Type::void_value:
        throw ScriptError("void values cannot be persisted");
    default:
        throw ScriptError("unknown Value type cannot be persisted");

    }
    std::vector<uint8_t> encoded;
    encoded.reserve(kStorageHeaderBytes + name.size() + payload_size);
    encoded.push_back(kStorageVersion);
    encoded.push_back(uint8_t(value.type));
    append_u16(encoded, uint16_t(name.size()));
    append_u32(encoded, uint32_t(payload_size));
    encoded.insert(encoded.end(), name.begin(), name.end());
    switch (value.type) {
    case Value::Type::number:
        append_u64(encoded, std::bit_cast<uint64_t>(static_cast<double>(value.number)));
        break;
    case Value::Type::boolean:
        encoded.push_back(value.boolean ? 1 : 0);
        break;
    case Value::Type::integer:
        append_u64(encoded, std::bit_cast<uint64_t>(value.integer));
        break;
    case Value::Type::string:
        encoded.insert(encoded.end(), value.string.begin(), value.string.end());
        break;
    case Value::Type::bytes:
        encoded.insert(encoded.end(), value.bytes->begin(), value.bytes->end());
        break;
    case Value::Type::handle:
    case Value::Type::void_value:
        break;
    default:
        throw ScriptError("unknown Value type cannot be persisted");
    }
    return encoded;
}

} // namespace

struct ZephyrPlatform::State final {
    explicit State(ZephyrHttpConfig config) {
        const int error = settings_subsys_init();
        if (error != 0) throw_settings_error("NVS initialization", error);
        logs.reserve(kRecordedLogEntries);
        http = std::make_unique<ZephyrHttp>(config);
    }

    std::unique_ptr<ZephyrHttp> http;
    std::vector<std::string> logs;
};

ZephyrPlatform::ZephyrPlatform(ZephyrHttpConfig http_config)
    : m_state(std::make_unique<State>(http_config)) {}
ZephyrPlatform::~ZephyrPlatform() = default;

void ZephyrPlatform::start_network() { m_state->http->start_network(); }
bool ZephyrPlatform::network_online() const { return m_state->http->network_online(); }
void ZephyrPlatform::stop_network() { m_state->http->shutdown(); }

void ZephyrPlatform::start_package_download(uint32_t id, std::string url, std::size_t maximum_bytes) {
    m_state->http->start_package(id, std::move(url), maximum_bytes);
}
bool ZephyrPlatform::poll_package_download(HttpResult& result) {
    return m_state->http->poll(ZephyrRequestKind::package, result);
}
bool ZephyrPlatform::cancel_package_download(uint32_t id) {
    return m_state->http->cancel(ZephyrRequestKind::package, id);
}

uint64_t ZephyrPlatform::now_ms() const { return uint64_t(k_uptime_get()); }

void ZephyrPlatform::start_request(uint32_t id, std::string url) {
    m_state->http->start(ZephyrRequestKind::script, id,
                         ContentRequest{std::move(url), ContentRequestMethod::get, {}});
}
bool ZephyrPlatform::poll_request(HttpResult& result) {
    return m_state->http->poll(ZephyrRequestKind::script, result);
}
void ZephyrPlatform::start_content(uint32_t id, ContentRequest request) {
    m_state->http->start(ZephyrRequestKind::content, id, std::move(request));
}
bool ZephyrPlatform::poll_content(HttpResult& result) {
    return m_state->http->poll(ZephyrRequestKind::content, result);
}
std::size_t ZephyrPlatform::log_count(std::string_view message) const {
    return static_cast<std::size_t>(std::count(m_state->logs.begin(), m_state->logs.end(), message));
}

std::optional<Value> ZephyrPlatform::load_value(const std::string& name) const {
    std::optional<StoredRecord> record = read_record(name);
    if (!record) return std::nullopt;
    return std::move(record->value);
}

void ZephyrPlatform::save_value(const std::string& name, const Value& value) {
    std::vector<uint8_t> encoded = encode_record(name, value);
    // Loading first detects a corrupt record or an FNV-1a collision rather than
    // replacing a different application's value at the same settings key.
    (void)read_record(name);
    const std::string key = settings_key(name);
    const int error = settings_save_one(key.c_str(), encoded.data(), encoded.size());
    if (error != 0) throw_settings_error("synchronous NVS write", error);
}

void ZephyrPlatform::log(std::string_view message) {
    std::array<char, kMaximumLogBytes + 1> sanitized{};
    const std::size_t count = std::min(message.size(), kMaximumLogBytes);
    for (std::size_t index = 0; index < count; ++index) {
        const unsigned char byte = static_cast<unsigned char>(message[index]);
        sanitized[index] = byte >= 0x20U && byte != 0x7fU ? char(byte) : '?';
    }
    if (message.size() > kMaximumLogBytes) sanitized[kMaximumLogBytes - 1] = '~';
    sanitized[count] = '\0';
    LOG_INF("application: %s", sanitized.data());
    if (m_state->logs.size() == kRecordedLogEntries) m_state->logs.erase(m_state->logs.begin());
    m_state->logs.emplace_back(sanitized.data(), count);
}

} // namespace mb
