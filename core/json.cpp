#include "microbrowser/json.h"

#include "microbrowser/package.h"

#include "reader.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace mb {
namespace {

constexpr std::size_t max_json_bytes = 64U * 1024U;
constexpr unsigned max_json_depth = 16;

struct JsonValue {
    enum class Type : uint8_t { null_value, boolean, number, string, object, array };

    Type type = Type::null_value;
    bool boolean = false;
    float number = 0.0F;
    std::string string;
    std::vector<std::string> keys;
    std::vector<JsonValue> children;
};

[[nodiscard]] bool is_digit(char value) {
    return value >= '0' && value <= '9';
}

void append_utf8(uint32_t codepoint, std::string& output) {
    if (codepoint <= 0x7fU) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else if (codepoint <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
}

class JsonParser {
public:
    explicit JsonParser(std::string_view document) : document_(document) {}

    JsonValue parse() {
        skip_whitespace();
        if (position_ == document_.size()) {
            fail("expected a JSON value");
        }
        JsonValue result = parse_value(0);
        skip_whitespace();
        if (position_ != document_.size()) {
            fail("trailing characters after the JSON value");
        }
        return result;
    }

private:
    [[noreturn]] void fail(const std::string& message) const {
        throw ScriptError("JSON error at byte " + std::to_string(position_) + ": " + message);
    }

    [[noreturn]] void fail_at(std::size_t position, const std::string& message) const {
        throw ScriptError("JSON error at byte " + std::to_string(position) + ": " + message);
    }

    void skip_whitespace() {
        while (position_ < document_.size()) {
            const char value = document_[position_];
            if (value != ' ' && value != '\t' && value != '\n' && value != '\r') {
                break;
            }
            ++position_;
        }
    }

    char take() {
        if (position_ == document_.size()) {
            fail("unexpected end of document");
        }
        return document_[position_++];
    }

    bool consume(char expected) {
        if (position_ < document_.size() && document_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void expect(char expected) {
        if (!consume(expected)) {
            fail(std::string("expected '") + expected + "'");
        }
    }

    JsonValue parse_value(unsigned depth) {
        if (position_ == document_.size()) {
            fail("expected a JSON value");
        }
        switch (document_[position_]) {
        case '{':
            if (depth == max_json_depth) {
                fail("nesting exceeds 16 levels");
            }
            return parse_object(depth + 1);
        case '[':
            if (depth == max_json_depth) {
                fail("nesting exceeds 16 levels");
            }
            return parse_array(depth + 1);
        case '"': {
            JsonValue result;
            result.type = JsonValue::Type::string;
            result.string = parse_string();
            return result;
        }
        case 't':
            return parse_literal("true", JsonValue::Type::boolean, true);
        case 'f':
            return parse_literal("false", JsonValue::Type::boolean, false);
        case 'n':
            return parse_literal("null", JsonValue::Type::null_value, false);
        default:
            if (document_[position_] == '-' || is_digit(document_[position_])) {
                return parse_number();
            }
            fail("expected a JSON value");
        }
    }

    JsonValue parse_literal(std::string_view literal, JsonValue::Type type, bool boolean) {
        if (document_.size() - position_ < literal.size() ||
            document_.substr(position_, literal.size()) != literal) {
            fail("invalid literal");
        }
        position_ += literal.size();
        JsonValue result;
        result.type = type;
        result.boolean = boolean;
        return result;
    }

    JsonValue parse_object(unsigned depth) {
        expect('{');
        JsonValue result;
        result.type = JsonValue::Type::object;
        skip_whitespace();
        if (consume('}')) {
            return result;
        }
        while (true) {
            skip_whitespace();
            if (position_ == document_.size() || document_[position_] != '"') {
                fail("expected a quoted object key");
            }
            result.keys.push_back(parse_string());
            skip_whitespace();
            expect(':');
            skip_whitespace();
            result.children.push_back(parse_value(depth));
            skip_whitespace();
            if (consume('}')) {
                return result;
            }
            expect(',');
        }
    }

    JsonValue parse_array(unsigned depth) {
        expect('[');
        JsonValue result;
        result.type = JsonValue::Type::array;
        skip_whitespace();
        if (consume(']')) {
            return result;
        }
        while (true) {
            result.children.push_back(parse_value(depth));
            skip_whitespace();
            if (consume(']')) {
                return result;
            }
            expect(',');
            skip_whitespace();
        }
    }

    int hex_digit(char value) const {
        if (value >= '0' && value <= '9') {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f') {
            return value - 'a' + 10;
        }
        if (value >= 'A' && value <= 'F') {
            return value - 'A' + 10;
        }
        return -1;
    }

    uint16_t parse_hex_quad() {
        uint16_t value = 0;
        for (unsigned index = 0; index < 4; ++index) {
            if (position_ == document_.size()) {
                fail("truncated Unicode escape");
            }
            const int digit = hex_digit(document_[position_]);
            if (digit < 0) {
                fail("invalid hexadecimal digit in Unicode escape");
            }
            value = static_cast<uint16_t>((value << 4U) | static_cast<unsigned>(digit));
            ++position_;
        }
        return value;
    }

    void parse_unicode_escape(std::string& output) {
        const uint16_t first = parse_hex_quad();
        uint32_t codepoint = first;
        if (first >= 0xd800U && first <= 0xdbffU) {
            if (document_.size() - position_ < 2 || document_[position_] != '\\' ||
                document_[position_ + 1] != 'u') {
                fail("high surrogate is not followed by a low surrogate");
            }
            position_ += 2;
            const uint16_t second = parse_hex_quad();
            if (second < 0xdc00U || second > 0xdfffU) {
                fail("high surrogate is not followed by a low surrogate");
            }
            codepoint = 0x10000U + ((static_cast<uint32_t>(first) - 0xd800U) << 10U) +
                        (static_cast<uint32_t>(second) - 0xdc00U);
        } else if (first >= 0xdc00U && first <= 0xdfffU) {
            fail("low surrogate has no preceding high surrogate");
        }
        append_utf8(codepoint, output);
    }

    void parse_escape(std::string& output) {
        const char escape = take();
        switch (escape) {
        case '"': output.push_back('"'); return;
        case '\\': output.push_back('\\'); return;
        case '/': output.push_back('/'); return;
        case 'b': output.push_back('\b'); return;
        case 'f': output.push_back('\f'); return;
        case 'n': output.push_back('\n'); return;
        case 'r': output.push_back('\r'); return;
        case 't': output.push_back('\t'); return;
        case 'u': parse_unicode_escape(output); return;
        default: fail("invalid string escape");
        }
    }

    void append_raw_utf8(std::string& output) {
        const std::size_t start = position_ - 1;
        const auto lead = static_cast<uint8_t>(document_[start]);
        std::size_t length = 0;
        uint8_t second_min = 0x80;
        uint8_t second_max = 0xbf;
        if (lead >= 0xc2 && lead <= 0xdf) {
            length = 2;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            length = 3;
            if (lead == 0xe0) {
                second_min = 0xa0;
            } else if (lead == 0xed) {
                second_max = 0x9f;
            }
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            length = 4;
            if (lead == 0xf0) {
                second_min = 0x90;
            } else if (lead == 0xf4) {
                second_max = 0x8f;
            }
        } else {
            fail_at(start, "invalid UTF-8 in string");
        }
        if (document_.size() - start < length) {
            fail_at(start, "truncated UTF-8 in string");
        }
        const auto second = static_cast<uint8_t>(document_[start + 1]);
        if (second < second_min || second > second_max) {
            fail_at(start, "invalid UTF-8 in string");
        }
        for (std::size_t index = 2; index < length; ++index) {
            const auto continuation = static_cast<uint8_t>(document_[start + index]);
            if (continuation < 0x80 || continuation > 0xbf) {
                fail_at(start, "invalid UTF-8 in string");
            }
        }
        output.append(document_.substr(start, length));
        position_ = start + length;
    }

    std::string parse_string() {
        expect('"');
        std::string result;
        while (true) {
            if (position_ == document_.size()) {
                fail("unterminated string");
            }
            const auto value = static_cast<uint8_t>(document_[position_++]);
            if (value == '"') {
                return result;
            }
            if (value < 0x20U) {
                fail_at(position_ - 1, "unescaped control character in string");
            }
            if (value == '\\') {
                parse_escape(result);
            } else if (value < 0x80U) {
                result.push_back(static_cast<char>(value));
            } else {
                append_raw_utf8(result);
            }
        }
    }

    JsonValue parse_number() {
        const std::size_t start = position_;
        consume('-');
        if (position_ == document_.size()) {
            fail("truncated number");
        }
        if (consume('0')) {
            if (position_ < document_.size() && is_digit(document_[position_])) {
                fail("leading zero in number");
            }
        } else {
            if (position_ == document_.size() || document_[position_] < '1' ||
                document_[position_] > '9') {
                fail("expected an integer digit");
            }
            while (position_ < document_.size() && is_digit(document_[position_])) {
                ++position_;
            }
        }
        if (consume('.')) {
            if (position_ == document_.size() || !is_digit(document_[position_])) {
                fail("expected a digit after decimal point");
            }
            while (position_ < document_.size() && is_digit(document_[position_])) {
                ++position_;
            }
        }
        if (position_ < document_.size() &&
            (document_[position_] == 'e' || document_[position_] == 'E')) {
            ++position_;
            if (position_ < document_.size() &&
                (document_[position_] == '+' || document_[position_] == '-')) {
                ++position_;
            }
            if (position_ == document_.size() || !is_digit(document_[position_])) {
                fail("expected an exponent digit");
            }
            while (position_ < document_.size() && is_digit(document_[position_])) {
                ++position_;
            }
        }
        const char* first = document_.data() + start;
        const char* last = document_.data() + position_;
        float number = 0.0F;
        const auto conversion = std::from_chars(first, last, number, std::chars_format::general);
        if (conversion.ec == std::errc::result_out_of_range || !std::isfinite(number)) {
            fail_at(start, "number is outside the finite binary32 range");
        }
        if (conversion.ec != std::errc{} || conversion.ptr != last) {
            fail_at(start, "invalid number");
        }
        JsonValue result;
        result.type = JsonValue::Type::number;
        result.number = number;
        return result;
    }

    std::string_view document_;
    std::size_t position_ = 0;
};

struct PathSegment {
    std::string_view text;
    std::string_view remaining;
};

PathSegment next_path_segment(std::string_view remaining) {
    const std::size_t dot = remaining.find('.');
    if (dot == std::string_view::npos) {
        return {remaining, {}};
    }
    if (dot + 1 == remaining.size()) {
        throw ScriptError("JSON path contains an empty segment");
    }
    return {remaining.substr(0, dot), remaining.substr(dot + 1)};
}

const JsonValue& resolve_path(const JsonValue& root, std::string_view path) {
    if (path.empty()) {
        return root;
    }
    const JsonValue* current = &root;
    std::string_view remaining = path;
    while (!remaining.empty()) {
        const PathSegment segment = next_path_segment(remaining);
        if (segment.text.empty()) {
            throw ScriptError("JSON path contains an empty segment");
        }
        if (current->type == JsonValue::Type::object) {
            const JsonValue* next = nullptr;
            for (std::size_t index = current->keys.size(); index > 0; --index) {
                if (current->keys[index - 1] == segment.text) {
                    next = &current->children[index - 1];
                    break;
                }
            }
            if (next == nullptr) {
                throw ScriptError("JSON path key '" + std::string(segment.text) + "' is missing");
            }
            current = next;
        } else if (current->type == JsonValue::Type::array) {
            std::size_t index = 0;
            for (char digit : segment.text) {
                if (!is_digit(digit)) {
                    throw ScriptError("JSON array path segment '" + std::string(segment.text) +
                                      "' is not a nonnegative integer");
                }
                const unsigned value = static_cast<unsigned>(digit - '0');
                if (index > (SIZE_MAX - value) / 10U) {
                    throw ScriptError("JSON array index is too large");
                }
                index = index * 10U + value;
            }
            if (index >= current->children.size()) {
                throw ScriptError("JSON array index " + std::to_string(index) + " is out of range");
            }
            current = &current->children[index];
        } else {
            throw ScriptError("JSON path traverses a scalar value");
        }
        remaining = segment.remaining;
    }
    return *current;
}

constexpr std::size_t max_reader_title_bytes = 512;
constexpr std::size_t max_reader_url_bytes = 2'047;
constexpr std::size_t max_reader_session_bytes = 256;
constexpr std::size_t max_reader_blocks = 256;
constexpr std::size_t max_reader_block_text_bytes = 8'192;
constexpr std::size_t max_reader_total_text_bytes = 24'000;
constexpr std::size_t max_reader_unsupported_reason_bytes = 512;
constexpr int max_reader_image_dimension = 16'384;

enum class NullPolicy : uint8_t { rejected, allowed };

[[nodiscard]] bool ascii_iequals(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        unsigned char a = static_cast<unsigned char>(left[index]);
        unsigned char b = static_cast<unsigned char>(right[index]);
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<unsigned char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<unsigned char>(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

class ReaderJsonConverter {
public:
    ReaderDocument convert(const JsonValue& root) const {
        require_object(root, "$");

        ReaderDocument result;
        if (required_string(root, "$", "format") != "microbrowser-reader-v1") {
            fail_field("$", "format", "must be exactly 'microbrowser-reader-v1'");
        }

        const std::string kind = required_string(root, "$", "kind");
        if (kind == "article") {
            result.kind = ReaderKind::article;
        } else if (kind == "manga") {
            result.kind = ReaderKind::manga;
        } else if (kind == "unsupported") {
            result.kind = ReaderKind::unsupported;
        } else {
            fail_field("$", "kind", "unknown reader document kind '" + kind + "'");
        }

        const std::string mode = required_string(root, "$", "mode");
        if (mode == "reader") {
            result.mode = ReaderMode::reader;
        } else if (mode == "interactive") {
            result.mode = ReaderMode::interactive;
        } else {
            fail_field("$", "mode", "unknown reader document mode '" + mode + "'");
        }

        result.title = bounded_required_string(root, "$", "title", max_reader_title_bytes);
        result.source_url = bounded_required_string(root, "$", "source_url", max_reader_url_bytes);
        require_url(result.source_url, "$", "source_url");
        result.previous_url = bounded_optional_string(root, "$", "previous_url",
                                                      max_reader_url_bytes, NullPolicy::allowed);
        if (result.previous_url) {
            require_url(*result.previous_url, "$", "previous_url");
        }
        result.next_url = bounded_optional_string(root, "$", "next_url",
                                                  max_reader_url_bytes, NullPolicy::allowed);
        if (result.next_url) {
            require_url(*result.next_url, "$", "next_url");
        }
        result.session = bounded_optional_string(root, "$", "session",
                                                 max_reader_session_bytes, NullPolicy::allowed);
        if (result.mode == ReaderMode::interactive &&
            (!result.session || result.session->empty())) {
            fail_field("$", "session", "interactive documents require a nonempty session");
        }

        if (const JsonValue* available = member(root, "$", "interactive_available")) {
            if (available->type != JsonValue::Type::boolean) {
                fail_field("$", "interactive_available", "expected boolean");
            }
            result.interactive_available = available->boolean;
        }
        result.unsupported_reason = bounded_optional_string(
            root, "$", "unsupported_reason", max_reader_unsupported_reason_bytes,
            NullPolicy::rejected);
        result.blocks = convert_blocks(required_member(root, "$", "blocks"));
        return result;
    }

private:
    [[noreturn]] static void fail(std::string_view path, const std::string& message) {
        throw PackageError("reader document " + std::string(path) + ": " + message);
    }

    [[noreturn]] static void fail_field(std::string_view object_path, std::string_view key,
                                        const std::string& message) {
        fail(std::string(object_path) + "." + std::string(key), message);
    }

    static void require_object(const JsonValue& value, std::string_view path) {
        if (value.type != JsonValue::Type::object) {
            fail(path, "expected object");
        }
    }

    static const JsonValue* member(const JsonValue& object, std::string_view object_path,
                                   std::string_view key) {
        const JsonValue* result = nullptr;
        for (std::size_t index = 0; index < object.keys.size(); ++index) {
            if (object.keys[index] != key) {
                continue;
            }
            if (result != nullptr) {
                fail_field(object_path, key, "duplicate object member");
            }
            result = &object.children[index];
        }
        return result;
    }

    static const JsonValue& required_member(const JsonValue& object,
                                            std::string_view object_path,
                                            std::string_view key) {
        const JsonValue* value = member(object, object_path, key);
        if (value == nullptr) {
            fail_field(object_path, key, "is required");
        }
        return *value;
    }

    static std::string required_string(const JsonValue& object, std::string_view object_path,
                                       std::string_view key) {
        const JsonValue& value = required_member(object, object_path, key);
        if (value.type != JsonValue::Type::string) {
            fail_field(object_path, key, "expected string");
        }
        return value.string;
    }

    static std::optional<std::string> optional_string(const JsonValue& object,
                                                      std::string_view object_path,
                                                      std::string_view key,
                                                      NullPolicy null_policy) {
        const JsonValue* value = member(object, object_path, key);
        const bool null_allowed = null_policy == NullPolicy::allowed;
        if (value == nullptr || (null_allowed && value->type == JsonValue::Type::null_value)) {
            return std::nullopt;
        }
        if (value->type != JsonValue::Type::string) {
            fail_field(object_path, key, "expected string" +
                       std::string(null_allowed ? " or null" : ""));
        }
        return value->string;
    }

    static void require_size(std::string_view value, std::size_t maximum,
                             std::string_view object_path, std::string_view key) {
        if (value.size() > maximum) {
            fail_field(object_path, key, "exceeds " + std::to_string(maximum) + " UTF-8 bytes");
        }
    }

    static std::string bounded_required_string(const JsonValue& object,
                                               std::string_view object_path,
                                               std::string_view key, std::size_t maximum) {
        std::string result = required_string(object, object_path, key);
        require_size(result, maximum, object_path, key);
        return result;
    }

    static std::optional<std::string> bounded_optional_string(
        const JsonValue& object, std::string_view object_path, std::string_view key,
        std::size_t maximum, NullPolicy null_policy) {
        std::optional<std::string> result = optional_string(object, object_path, key, null_policy);
        if (result) {
            require_size(*result, maximum, object_path, key);
        }
        return result;
    }

    static void require_url(std::string_view url, std::string_view object_path,
                            std::string_view key) {
        if (url.empty()) {
            fail_field(object_path, key, "must be nonempty");
        }
        validate_url(url, object_path, key);
    }

    static void validate_url(std::string_view url, std::string_view object_path,
                             std::string_view key) {
        if (url.empty()) {
            return;
        }
        for (const unsigned char byte : url) {
            if (byte <= 0x20U || byte == 0x7fU) {
                fail_field(object_path, key, "contains whitespace or a control character");
            }
        }

        const std::size_t colon = url.find(':');
        const std::size_t delimiter = url.find_first_of("/?#");
        if (colon == std::string_view::npos ||
            (delimiter != std::string_view::npos && delimiter < colon)) {
            return;
        }
        const std::string_view scheme = url.substr(0, colon);
        bool valid_scheme = !scheme.empty() &&
            ((scheme.front() >= 'A' && scheme.front() <= 'Z') ||
             (scheme.front() >= 'a' && scheme.front() <= 'z'));
        for (const char character : scheme.substr(valid_scheme ? 1 : 0)) {
            const bool alphanumeric = (character >= 'A' && character <= 'Z') ||
                (character >= 'a' && character <= 'z') ||
                (character >= '0' && character <= '9');
            valid_scheme = valid_scheme && (alphanumeric || character == '+' ||
                                            character == '-' || character == '.');
        }
        if (!valid_scheme) {
            fail_field(object_path, key, "has an invalid URL scheme");
        }
        if (!ascii_iequals(scheme, "http") && !ascii_iequals(scheme, "https")) {
            fail_field(object_path, key, "URL scheme must be HTTP or HTTPS");
        }
    }

    static std::optional<int> optional_dimension(const JsonValue& object,
                                                 std::string_view object_path,
                                                 std::string_view key) {
        const JsonValue* value = member(object, object_path, key);
        if (value == nullptr) {
            return std::nullopt;
        }
        if (value->type != JsonValue::Type::number) {
            fail_field(object_path, key, "expected number");
        }
        if (std::trunc(value->number) != value->number || value->number < 1.0F ||
            value->number > float(max_reader_image_dimension)) {
            fail_field(object_path, key, "must be a positive integer no greater than 16384");
        }
        return static_cast<int>(value->number);
    }

    static std::vector<ReaderBlock> convert_blocks(const JsonValue& value) {
        if (value.type != JsonValue::Type::array) {
            fail("$.blocks", "expected array");
        }
        if (value.children.size() > max_reader_blocks) {
            fail("$.blocks", "contains more than 256 blocks");
        }

        std::vector<ReaderBlock> result;
        result.reserve(value.children.size());
        std::size_t total_text_bytes = 0;
        for (std::size_t index = 0; index < value.children.size(); ++index) {
            const std::string path = "$.blocks[" + std::to_string(index) + "]";
            const JsonValue& item = value.children[index];
            require_object(item, path);
            result.push_back(convert_block(item, path, total_text_bytes));
        }
        return result;
    }

    static ReaderBlock convert_block(const JsonValue& item, const std::string& path,
                                     std::size_t& total_text_bytes) {
        ReaderBlock result;
        const std::string type = required_string(item, path, "type");
        bool text_required = true;
        bool url_required = false;
        if (type == "heading") {
            result.kind = ReaderBlockKind::heading;
        } else if (type == "paragraph") {
            result.kind = ReaderBlockKind::paragraph;
        } else if (type == "text") {
            result.kind = ReaderBlockKind::notice;
        } else if (type == "link") {
            result.kind = ReaderBlockKind::link;
            text_required = false;
            url_required = true;
        } else if (type == "image") {
            result.kind = ReaderBlockKind::image;
            text_required = false;
            url_required = true;
        } else {
            fail_field(path, "type", "unknown reader block type '" + type + "'");
        }

        if (text_required) {
            result.text = bounded_required_string(item, path, "text",
                                                  max_reader_block_text_bytes);
        } else if (std::optional<std::string> text = bounded_optional_string(
                       item, path, "text", max_reader_block_text_bytes, NullPolicy::rejected)) {
            result.text = std::move(*text);
        }
        if (result.text.size() > max_reader_total_text_bytes - total_text_bytes) {
            fail_field(path, "text", "total block text exceeds 24000 UTF-8 bytes");
        }
        total_text_bytes += result.text.size();

        if (url_required) {
            result.url = bounded_required_string(item, path, "url", max_reader_url_bytes);
        } else if (std::optional<std::string> url = bounded_optional_string(
                       item, path, "url", max_reader_url_bytes, NullPolicy::rejected)) {
            result.url = std::move(*url);
        }
        if (url_required) {
            require_url(result.url, path, "url");
        } else {
            validate_url(result.url, path, "url");
        }
        result.width = optional_dimension(item, path, "width");
        result.height = optional_dimension(item, path, "height");
        return result;
    }
};

} // namespace

Value read_json_scalar(std::string_view document, std::string_view path,
                       Value::Type expected_type) {
    if (document.size() > max_json_bytes) {
        throw ScriptError("JSON document exceeds 64 KiB");
    }
    if (expected_type != Value::Type::number && expected_type != Value::Type::boolean &&
        expected_type != Value::Type::string) {
        throw ScriptError("JSON reader requires a number, boolean, or string result type");
    }

    const JsonValue root = JsonParser(document).parse();
    const JsonValue& selected = resolve_path(root, path);
    Value result;
    result.type = expected_type;
    switch (expected_type) {
    case Value::Type::number:
        if (selected.type != JsonValue::Type::number) {
            throw ScriptError("JSON path selects a value of the wrong type; expected number");
        }
        result.number = selected.number;
        break;
    case Value::Type::boolean:
        if (selected.type != JsonValue::Type::boolean) {
            throw ScriptError("JSON path selects a value of the wrong type; expected boolean");
        }
        result.boolean = selected.boolean;
        break;
    case Value::Type::string:
        if (selected.type != JsonValue::Type::string) {
            throw ScriptError("JSON path selects a value of the wrong type; expected string");
        }
        result.string = selected.string;
        break;
    case Value::Type::integer:
    case Value::Type::bytes:
    case Value::Type::handle:
        throw ScriptError("JSON reader requires a number, boolean, or string result type");
    case Value::Type::void_value:
        break;
    }
    return result;
}

ReaderDocument parse_reader_document(std::string_view json) {
    if (json.size() > max_json_bytes) {
        throw PackageError("JSON document exceeds 64 KiB");
    }
    try {
        const JsonValue root = JsonParser(json).parse();
        return ReaderJsonConverter().convert(root);
    } catch (const ScriptError& error) {
        throw PackageError(error.what());
    }
}

} // namespace mb
