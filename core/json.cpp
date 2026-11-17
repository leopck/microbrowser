#include "microbrowser/json.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
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
    case Value::Type::void_value:
        break;
    }
    return result;
}

} // namespace mb
