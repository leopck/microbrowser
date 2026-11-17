#include "microbrowser/package.h"

#include "fonts.h"

#include <utility>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mb {
namespace {

constexpr std::size_t max_package_bytes = 16U * 1024U * 1024U;
constexpr std::size_t max_states = 256;
constexpr std::size_t max_functions = 128;
constexpr std::size_t max_locals = 32;
constexpr std::size_t max_constants = 256;
constexpr std::size_t max_code_bytes = 65'535;
constexpr std::size_t max_pages = 32;
constexpr std::size_t max_nodes = 2'048;
constexpr std::size_t max_node_depth = 32;
constexpr std::size_t max_node_children = 64;
constexpr std::size_t max_properties = 40;
constexpr std::size_t max_asset_dimension = 2'048;
constexpr int max_geometry_pixels = 16'384;

class ByteReader {
public:
    ByteReader(const std::vector<uint8_t>& bytes, std::size_t begin, std::size_t end,
               std::string context)
        : bytes_(bytes), position_(begin), end_(end), context_(std::move(context)) {}

    [[nodiscard]] std::size_t position() const { return position_; }
    [[nodiscard]] std::size_t remaining() const { return end_ - position_; }

    [[noreturn]] void fail(const std::string& message) const {
        throw PackageError(context_ + " at byte " + std::to_string(position_) + ": " + message);
    }

    [[noreturn]] void fail_at(std::size_t position, const std::string& message) const {
        throw PackageError(context_ + " at byte " + std::to_string(position) + ": " + message);
    }

    void require(std::size_t size, std::string_view description) const {
        if (size > remaining()) {
            fail("truncated " + std::string(description));
        }
    }

    uint8_t read_u8() {
        require(1, "byte");
        return bytes_[position_++];
    }

    uint16_t read_u16() {
        require(2, "16-bit integer");
        const uint16_t result = static_cast<uint16_t>(bytes_[position_]) |
                                static_cast<uint16_t>(static_cast<uint16_t>(bytes_[position_ + 1])
                                                      << 8U);
        position_ += 2;
        return result;
    }

    uint32_t read_u32() {
        require(4, "32-bit integer");
        const uint32_t result = static_cast<uint32_t>(bytes_[position_]) |
                                (static_cast<uint32_t>(bytes_[position_ + 1]) << 8U) |
                                (static_cast<uint32_t>(bytes_[position_ + 2]) << 16U) |
                                (static_cast<uint32_t>(bytes_[position_ + 3]) << 24U);
        position_ += 4;
        return result;
    }

    float read_float() {
        const float result = std::bit_cast<float>(read_u32());
        if (!std::isfinite(result)) {
            fail_at(position_ - 4, "non-finite binary32 value");
        }
        return result;
    }

    std::span<const uint8_t> read_bytes(std::size_t size) {
        require(size, "byte range");
        const std::span<const uint8_t> result(bytes_.data() + position_, size);
        position_ += size;
        return result;
    }

    ByteReader read_nested(std::size_t size, std::string context) {
        require(size, "section payload");
        ByteReader result(bytes_, position_, position_ + size, std::move(context));
        position_ += size;
        return result;
    }

    void finish() const {
        if (remaining() != 0) {
            fail("trailing bytes in section");
        }
    }

private:
    const std::vector<uint8_t>& bytes_;
    std::size_t position_;
    std::size_t end_;
    std::string context_;
};

[[nodiscard]] std::string hex_byte(uint8_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.push_back(digits[value >> 4U]);
    result.push_back(digits[value & 0x0fU]);
    return result;
}

[[nodiscard]] std::string section_description(const std::array<uint8_t, 4>& identifier) {
    std::string result;
    for (uint8_t byte : identifier) {
        result += byte >= 0x20 && byte <= 0x7e ? std::string(1, static_cast<char>(byte))
                                               : "\\x" + hex_byte(byte);
    }
    return result;
}

void validate_utf8(std::string_view value, std::string_view description) {
    std::size_t position = 0;
    const auto fail = [&](std::string_view reason) {
        throw PackageError(std::string(description) + " at string byte " + std::to_string(position) +
                           ": " + std::string(reason));
    };
    while (position < value.size()) {
        const auto lead = static_cast<uint8_t>(value[position]);
        if (lead < 0x80U) {
            ++position;
            continue;
        }
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
            fail("invalid UTF-8 leading byte");
        }
        if (length > value.size() - position) {
            fail("truncated UTF-8 sequence");
        }
        const auto second = static_cast<uint8_t>(value[position + 1]);
        if (second < second_min || second > second_max) {
            fail("invalid or non-shortest UTF-8 sequence");
        }
        for (std::size_t index = 2; index < length; ++index) {
            const auto continuation = static_cast<uint8_t>(value[position + index]);
            if (continuation < 0x80 || continuation > 0xbf) {
                fail("invalid UTF-8 continuation byte");
            }
        }
        position += length;
    }
}

std::string_view string_at(const Package& package, uint32_t index, std::string_view description) {
    if (index >= package.strings.size()) {
        throw PackageError(std::string(description) + ": string ID " + std::to_string(index) +
                           " is out of range");
    }
    return package.strings[index];
}

void parse_strings(ByteReader& reader, Package& package) {
    const uint32_t count = reader.read_u32();
    const uint64_t table_bytes = static_cast<uint64_t>(count) * 4U;
    if (table_bytes > reader.remaining()) {
        reader.fail("string offset table exceeds STRS payload");
    }
    std::vector<uint32_t> offsets;
    offsets.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        offsets.push_back(reader.read_u32());
    }
    const std::span<const uint8_t> bytes = reader.read_bytes(reader.remaining());
    const std::string_view region = bytes.empty()
                                        ? std::string_view{}
                                        : std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                                           bytes.size());
    package.strings.reserve(count);
    std::size_t expected_offset = 0;
    for (uint32_t index = 0; index < count; ++index) {
        if (offsets[index] != expected_offset) {
            throw PackageError("STRS: string " + std::to_string(index) +
                               " has a noncontiguous or out-of-order offset");
        }
        if (expected_offset >= region.size()) {
            throw PackageError("STRS: string " + std::to_string(index) +
                               " starts outside the string data");
        }
        const std::size_t terminator = region.find('\0', expected_offset);
        if (terminator == std::string_view::npos) {
            throw PackageError("STRS: string " + std::to_string(index) + " has no NUL terminator");
        }
        const std::string_view value = region.substr(expected_offset, terminator - expected_offset);
        validate_utf8(value, "STRS string " + std::to_string(index));
        package.strings.emplace_back(value);
        expected_offset = terminator + 1;
    }
    if (expected_offset != region.size()) {
        throw PackageError("STRS: trailing bytes after the final NUL-terminated string");
    }
}

Value parse_typed_value(ByteReader& reader, const Package& package, std::string_view description) {
    const uint8_t tag = reader.read_u8();
    Value result;
    switch (tag) {
    case 0:
        result.type = Value::Type::number;
        result.number = reader.read_float();
        break;
    case 1: {
        result.type = Value::Type::boolean;
        const uint8_t boolean = reader.read_u8();
        if (boolean > 1) {
            reader.fail_at(reader.position() - 1, "boolean value must be zero or one");
        }
        result.boolean = boolean != 0;
        break;
    }
    case 2: {
        result.type = Value::Type::string;
        const uint32_t index = reader.read_u32();
        result.string = string_at(package, index, description);
        break;
    }
    default:
        reader.fail_at(reader.position() - 1, "unknown value kind " + std::to_string(tag));
    }
    return result;
}

void parse_states(ByteReader& reader, Package& package) {
    const uint32_t count = reader.read_u32();
    if (count > max_states) {
        reader.fail("STAT count exceeds 256 fields");
    }
    package.states.reserve(count);
    std::unordered_set<std::string> names;
    for (uint32_t index = 0; index < count; ++index) {
        const uint32_t name_id = reader.read_u32();
        const std::string name(string_at(package, name_id, "STAT field name"));
        if (!names.insert(name).second) {
            reader.fail("duplicate state name '" + name + "'");
        }
        Value initial = parse_typed_value(reader, package, "STAT field '" + name + "'");
        package.states.push_back(StateField{name, std::move(initial)});
    }
    reader.finish();
}

void require_code_bytes(const Function& function, std::size_t pc, std::size_t size) {
    if (pc >= function.code.size() || size > function.code.size() - pc) {
        throw PackageError("CODE function '" + function.name + "' at pc " + std::to_string(pc) +
                           ": truncated instruction immediate");
    }
}

uint16_t code_u16(const Function& function, std::size_t offset) {
    return static_cast<uint16_t>(function.code[offset]) |
           static_cast<uint16_t>(static_cast<uint16_t>(function.code[offset + 1]) << 8U);
}

unsigned known_builtin_arity(uint8_t id) {
    constexpr std::array<uint8_t, 29> arities = {
        2, 2, 3, 1, 1, 1, 1, 1, 0, 4, 2, 2, 1, 2, 2,
        2, 2, 2, 3, 2, 2, 1, 1, 1, 1, 2, 2, 2, 2,
    };
    return arities[id];
}

void validate_function_code(const Package& package, const Function& function) {
    std::vector<uint8_t> boundaries(function.code.size() + 1, 0);
    std::vector<std::pair<std::size_t, std::size_t>> jumps;
    const auto fail = [&](std::size_t pc, const std::string& message) {
        throw PackageError("CODE function '" + function.name + "' at pc " + std::to_string(pc) +
                           ": " + message);
    };
    std::size_t pc = 0;
    while (pc < function.code.size()) {
        const uint8_t opcode = function.code[pc];
        if (opcode == 0 || opcode > 24) {
            fail(pc, "unknown opcode " + std::to_string(opcode));
        }
        std::size_t size = 1;
        if (opcode == 1 || (opcode >= 3 && opcode <= 6) || opcode == 20 || opcode == 21) {
            size = 3;
        } else if (opcode == 2 || opcode == 23) {
            size = opcode == 2 ? 2 : 3;
        } else if (opcode == 22) {
            size = 4;
        }
        require_code_bytes(function, pc, size);
        boundaries[pc] = 1;
        switch (opcode) {
        case 1:
            if (code_u16(function, pc + 1) >= function.constants.size()) {
                fail(pc, "constant index is out of range");
            }
            break;
        case 3:
        case 4:
            if (code_u16(function, pc + 1) >= package.states.size()) {
                fail(pc, "state index is out of range");
            }
            break;
        case 5:
        case 6:
            if (code_u16(function, pc + 1) >= function.local_count) {
                fail(pc, "local index is out of range");
            }
            break;
        case 2:
            if (function.code[pc + 1] > 1) {
                fail(pc, "PUSH_BOOL immediate must be zero or one");
            }
            break;
        case 20:
        case 21: {
            const uint16_t bits = code_u16(function, pc + 1);
            const int32_t displacement = bits <= INT16_MAX ? static_cast<int32_t>(bits)
                                                           : static_cast<int32_t>(bits) - 65'536;
            const int64_t target = static_cast<int64_t>(pc + size) + displacement;
            if (target < 0 || target >= static_cast<int64_t>(function.code.size())) {
                fail(pc, "jump target is outside the function");
            }
            jumps.emplace_back(pc, static_cast<std::size_t>(target));
            break;
        }
        case 22: {
            const uint16_t target = code_u16(function, pc + 1);
            if (target >= package.functions.size()) {
                fail(pc, "function index is out of range");
            }
            if (function.code[pc + 3] != package.functions[target].parameter_count) {
                fail(pc, "CALL argument count does not match target arity");
            }
            break;
        }
        case 23:
            if (function.code[pc + 1] > 28) {
                fail(pc, "unknown builtin ID " + std::to_string(function.code[pc + 1]));
            }
            if (function.code[pc + 2] != known_builtin_arity(function.code[pc + 1])) {
                fail(pc, "BUILTIN argument count does not match its signature");
            }
            break;
        default:
            break;
        }
        pc += size;
    }
    for (const auto& [source, target] : jumps) {
        if (boundaries[target] == 0) {
            fail(source, "jump target is not on an instruction boundary");
        }
    }
}

void parse_functions(ByteReader& reader, Package& package) {
    const uint32_t count = reader.read_u32();
    if (count > max_functions) {
        reader.fail("CODE function count exceeds 128");
    }
    package.functions.reserve(count);
    std::unordered_set<std::string> names;
    for (uint32_t index = 0; index < count; ++index) {
        Function function;
        const uint32_t name_id = reader.read_u32();
        function.name = string_at(package, name_id, "CODE function name");
        if (!names.insert(function.name).second) {
            reader.fail("duplicate function name '" + function.name + "'");
        }
        function.parameter_count = reader.read_u16();
        function.local_count = reader.read_u16();
        if (function.local_count > max_locals) {
            reader.fail("function '" + function.name + "' has more than 32 locals");
        }
        if (function.parameter_count > function.local_count) {
            reader.fail("function '" + function.name + "' has more parameters than locals");
        }
        const uint32_t code_size = reader.read_u32();
        if (code_size > max_code_bytes) {
            reader.fail("function '" + function.name + "' exceeds 65,535 code bytes");
        }
        const uint32_t constant_count = reader.read_u32();
        if (constant_count > max_constants) {
            reader.fail("function '" + function.name + "' has more than 256 constants");
        }
        function.constants.reserve(constant_count);
        for (uint32_t constant = 0; constant < constant_count; ++constant) {
            function.constants.push_back(
                parse_typed_value(reader, package, "constant in function '" + function.name + "'"));
        }
        const std::span<const uint8_t> code = reader.read_bytes(code_size);
        function.code.assign(code.begin(), code.end());
        package.functions.push_back(std::move(function));
    }
    reader.finish();
    for (const Function& function : package.functions) {
        validate_function_code(package, function);
    }
}

enum class PropertyKind : uint8_t { string = 0, number = 1, boolean = 2, color = 3, state = 4 };

struct Property {
    std::string_view name;
    PropertyKind kind = PropertyKind::string;
    std::string_view string;
    float number = 0.0F;
    bool boolean = false;
    uint32_t integer = 0;
    std::size_t offset = 0;
};

Property read_property(ByteReader& reader, const Package& package) {
    Property property;
    property.offset = reader.position();
    property.name = string_at(package, reader.read_u32(), "PAGE property name");
    const uint8_t kind = reader.read_u8();
    if (kind > static_cast<uint8_t>(PropertyKind::state)) {
        reader.fail_at(reader.position() - 1, "unknown property value kind " + std::to_string(kind));
    }
    property.kind = static_cast<PropertyKind>(kind);
    switch (property.kind) {
    case PropertyKind::string:
        property.string = string_at(package, reader.read_u32(), "PAGE property '" +
                                                                   std::string(property.name) + "'");
        break;
    case PropertyKind::number:
        property.number = reader.read_float();
        break;
    case PropertyKind::boolean: {
        const uint8_t value = reader.read_u8();
        if (value > 1) {
            reader.fail_at(reader.position() - 1, "boolean property must be zero or one");
        }
        property.boolean = value != 0;
        break;
    }
    case PropertyKind::color:
    case PropertyKind::state:
        property.integer = reader.read_u32();
        break;
    }
    if (property.kind == PropertyKind::state && property.integer >= package.states.size()) {
        reader.fail_at(property.offset, "state index " + std::to_string(property.integer) +
                                            " is out of range");
    }
    return property;
}

void require_property_kind(const Property& property, PropertyKind expected, ByteReader& reader) {
    if (property.kind != expected) {
        reader.fail_at(property.offset, "property '" + std::string(property.name) +
                                            "' has the wrong value kind");
    }
}

[[nodiscard]] bool decimal_syntax(std::string_view text) {
    std::size_t position = 0;
    if (position < text.size() && text[position] == '-') {
        ++position;
    }
    std::size_t digits = 0;
    while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
        ++position;
        ++digits;
    }
    if (position < text.size() && text[position] == '.') {
        ++position;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
            ++position;
            ++digits;
        }
    }
    if (digits == 0) {
        return false;
    }
    if (position < text.size() && (text[position] == 'e' || text[position] == 'E')) {
        ++position;
        if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
            ++position;
        }
        const std::size_t exponent_start = position;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
            ++position;
        }
        if (position == exponent_start) {
            return false;
        }
    }
    return position == text.size();
}

float parse_finite_float(std::string_view text, std::string_view description) {
    if (!decimal_syntax(text)) {
        throw PackageError(std::string(description) + ": invalid decimal number");
    }
    float result = 0.0F;
    const auto conversion = std::from_chars(text.data(), text.data() + text.size(), result,
                                            std::chars_format::general);
    if (conversion.ec == std::errc::result_out_of_range || !std::isfinite(result)) {
        throw PackageError(std::string(description) + ": number is outside finite binary32 range");
    }
    if (conversion.ec != std::errc{} || conversion.ptr != text.data() + text.size()) {
        throw PackageError(std::string(description) + ": invalid decimal number");
    }
    return result;
}

int integer_property(const Property& property, int minimum, int maximum, ByteReader& reader) {
    require_property_kind(property, PropertyKind::number, reader);
    const double value = property.number;
    if (std::trunc(value) != value || value < minimum || value > maximum) {
        reader.fail_at(property.offset, "property '" + std::string(property.name) +
                                            "' must be an integer in the allowed range");
    }
    return static_cast<int>(value);
}

Length parse_length(const Property& property, ByteReader& reader) {
    Length result;
    if (property.kind == PropertyKind::number) {
        if (property.number < 0.0F || property.number > max_geometry_pixels) {
            reader.fail_at(property.offset, "fixed length must be between 0 and 16,384 pixels");
        }
        result.kind = LengthKind::fixed;
        result.value = property.number;
        return result;
    }
    require_property_kind(property, PropertyKind::string, reader);
    if (property.string == "auto") {
        result.kind = LengthKind::automatic;
        return result;
    }
    if (property.string == "grow") {
        result.kind = LengthKind::grow;
        return result;
    }
    const bool percent = !property.string.empty() && property.string.back() == '%';
    const std::string_view number = percent ? property.string.substr(0, property.string.size() - 1)
                                            : property.string;
    try {
        result.value = parse_finite_float(number, "PAGE length");
    } catch (const PackageError& error) {
        reader.fail_at(property.offset, error.what());
    }
    if (result.value < 0.0F || (percent && result.value > 100.0F)) {
        reader.fail_at(property.offset, "length is negative or percentage exceeds 100%");
    }
    if (!percent && result.value > max_geometry_pixels) {
        reader.fail_at(property.offset, "fixed length cannot exceed 16,384 pixels");
    }
    result.kind = percent ? LengthKind::percent : LengthKind::fixed;
    return result;
}

void validate_animation(std::string_view value, std::size_t offset, ByteReader& reader) {
    const std::size_t colon = value.find(':');
    const std::string_view name = value.substr(0, colon);
    if (name != "fade" && name != "pulse" && name != "slide") {
        reader.fail_at(offset, "animate must be fade, pulse, or slide");
    }
    if (colon == std::string_view::npos) {
        return;
    }
    const std::string_view duration = value.substr(colon + 1);
    if (duration.empty()) {
        reader.fail_at(offset, "animation duration is missing");
    }
    uint64_t milliseconds = 0;
    for (char digit : duration) {
        if (digit < '0' || digit > '9') {
            reader.fail_at(offset, "animation duration must be a positive integer");
        }
        const unsigned value_digit = static_cast<unsigned>(digit - '0');
        if (milliseconds > (std::numeric_limits<uint64_t>::max() - value_digit) / 10U) {
            reader.fail_at(offset, "animation duration is too large");
        }
        milliseconds = milliseconds * 10U + value_digit;
    }
    if (milliseconds == 0) {
        reader.fail_at(offset, "animation duration must be positive");
    }
}

bool apply_common_property(Node& node, const Property& property, ByteReader& reader) {
    const std::string_view name = property.name;
    if (name == "id") {
        require_property_kind(property, PropertyKind::string, reader);
        if (property.string.empty()) {
            reader.fail_at(property.offset, "element ID cannot be empty");
        }
        node.id = property.string;
    } else if (name == "animate") {
        require_property_kind(property, PropertyKind::string, reader);
        validate_animation(property.string, property.offset, reader);
        node.animation = property.string;
    } else if (name == "width") {
        node.style.width = parse_length(property, reader);
    } else if (name == "height") {
        node.style.height = parse_length(property, reader);
    } else if (name == "padding") {
        node.style.padding = integer_property(property, 0, std::numeric_limits<int>::max(), reader);
    } else if (name == "gap") {
        node.style.gap = integer_property(property, 0, std::numeric_limits<int>::max(), reader);
    } else if (name == "font-size" || name == "font-weight") {
        const int value = integer_property(property, 0, std::numeric_limits<int>::max(), reader);
        const int pixel_size = name == "font-size" ? value : node.style.font_size;
        const int weight = name == "font-weight" ? value : node.style.font_weight;
        if (!fonts::face_exists(pixel_size, weight)) {
            reader.fail_at(property.offset, "unsupported font size or weight");
        }
        if (name == "font-size") node.style.font_size = value;
        else node.style.font_weight = value;
    } else if (name == "radius") {
        node.style.radius = integer_property(property, 0, std::numeric_limits<int>::max(), reader);
    } else if (name == "x") {
        node.style.x = integer_property(property, -max_geometry_pixels, max_geometry_pixels, reader);
    } else if (name == "y") {
        node.style.y = integer_property(property, -max_geometry_pixels, max_geometry_pixels, reader);
    } else if (name == "scroll") {
        require_property_kind(property, PropertyKind::boolean, reader);
        node.style.scroll = property.boolean;
    } else if (name == "position") {
        require_property_kind(property, PropertyKind::string, reader);
        if (property.string == "static") {
            node.style.absolute = false;
        } else if (property.string == "absolute") {
            node.style.absolute = true;
        } else {
            reader.fail_at(property.offset, "position must be static or absolute");
        }
    } else if (name == "align") {
        require_property_kind(property, PropertyKind::string, reader);
        if (property.string == "start") {
            node.style.align = Align::start;
        } else if (property.string == "center") {
            node.style.align = Align::center;
        } else if (property.string == "end") {
            node.style.align = Align::end_position;
        } else if (property.string == "stretch") {
            node.style.align = Align::stretch;
        } else {
            reader.fail_at(property.offset, "unknown align value");
        }
    } else if (name == "justify") {
        require_property_kind(property, PropertyKind::string, reader);
        if (property.string == "start") {
            node.style.justify = Justify::start;
        } else if (property.string == "center") {
            node.style.justify = Justify::center;
        } else if (property.string == "end") {
            node.style.justify = Justify::end_position;
        } else if (property.string == "space-between") {
            node.style.justify = Justify::space_between;
        } else {
            reader.fail_at(property.offset, "unknown justify value");
        }
    } else if (name == "background" || name == "color" || name == "border") {
        require_property_kind(property, PropertyKind::color, reader);
        const Color color{property.integer};
        if (name == "background") {
            node.style.background = color;
        } else if (name == "color") {
            node.style.foreground = color;
        } else {
            node.style.border = color;
        }
    } else {
        return false;
    }
    return true;
}

[[nodiscard]] bool is_container(NodeKind kind) {
    return kind == NodeKind::view || kind == NodeKind::row || kind == NodeKind::column ||
           kind == NodeKind::stack;
}

[[nodiscard]] bool is_bindable(NodeKind kind) {
    return kind == NodeKind::text || kind == NodeKind::input || kind == NodeKind::slider ||
           kind == NodeKind::progress;
}

struct NodeFlags {
    bool has_content = false;
    bool has_binding = false;
    bool has_format = false;
};

void assign_text(Node& node, const Property& property, NodeFlags& flags, ByteReader& reader) {
    require_property_kind(property, PropertyKind::string, reader);
    if (flags.has_content) {
        reader.fail_at(property.offset, "element has more than one text or binding property");
    }
    node.text = property.string;
    flags.has_content = true;
}

void assign_binding(Node& node, const Property& property, const Package& package, NodeFlags& flags,
                    ByteReader& reader) {
    require_property_kind(property, PropertyKind::state, reader);
    if (flags.has_content || flags.has_binding) {
        reader.fail_at(property.offset, "element has more than one text or binding property");
    }
    const Value::Type state_type = package.states[property.integer].initial.type;
    if (node.kind == NodeKind::input && state_type != Value::Type::string) {
        reader.fail_at(property.offset, "input binding must reference a string state");
    }
    if ((node.kind == NodeKind::slider || node.kind == NodeKind::progress) &&
        state_type != Value::Type::number) {
        reader.fail_at(property.offset, "slider/progress binding must reference a numeric state");
    }
    node.binding = property.integer;
    flags.has_content = true;
    flags.has_binding = true;
}

uint32_t event_function(const Property& property, const Package& package, ByteReader& reader) {
    require_property_kind(property, PropertyKind::number, reader);
    const double value = property.number;
    if (std::trunc(value) != value || value < 0.0 || value >= 4294967296.0) {
        reader.fail_at(property.offset, "event function ID must be a nonnegative integer");
    }
    const uint32_t index = static_cast<uint32_t>(value);
    if (index >= package.functions.size()) {
        reader.fail_at(property.offset, "event function index is out of range");
    }
    if (package.functions[index].parameter_count != 0) {
        reader.fail_at(property.offset, "event function must take no arguments");
    }
    return index;
}

void apply_content_property(Node& node, const Property& property, const Package& package,
                            bool root, NodeFlags& flags, ByteReader& reader) {
    const std::string_view name = property.name;
    const bool binding_alias = name == "bind" || name == "value" || name == "text";
    if (is_bindable(node.kind) && binding_alias) {
        if (node.kind == NodeKind::text && name == "text" && property.kind == PropertyKind::string) {
            assign_text(node, property, flags, reader);
        } else if (node.kind == NodeKind::text && name == "value" &&
                   property.kind == PropertyKind::string) {
            assign_text(node, property, flags, reader);
        } else {
            assign_binding(node, property, package, flags, reader);
        }
        return;
    }
    if (node.kind == NodeKind::button && name == "text") {
        assign_text(node, property, flags, reader);
    } else if (node.kind == NodeKind::text && name == "format") {
        require_property_kind(property, PropertyKind::string, reader);
        node.format = property.string;
        flags.has_format = true;
    } else if (node.kind == NodeKind::input && name == "placeholder") {
        require_property_kind(property, PropertyKind::string, reader);
        node.placeholder = property.string;
    } else if ((node.kind == NodeKind::input) && name == "max-length") {
        node.max_length = static_cast<uint16_t>(integer_property(property, 0, 65'535, reader));
    } else if (((node.kind == NodeKind::slider || node.kind == NodeKind::progress) &&
                (name == "min" || name == "max" || name == "step")) ||
               (node.kind == NodeKind::chart && (name == "min" || name == "max"))) {
        require_property_kind(property, PropertyKind::number, reader);
        if (name == "min") {
            node.minimum = property.number;
        } else if (name == "max") {
            node.maximum = property.number;
        } else if (node.kind == NodeKind::slider) {
            node.step = property.number;
        } else {
            reader.fail_at(property.offset, "progress does not accept step");
        }
    } else if (node.kind == NodeKind::button && name == "on_press") {
        node.on_press = event_function(property, package, reader);
    } else if ((node.kind == NodeKind::input || node.kind == NodeKind::slider) &&
               name == "on_change") {
        node.on_change = event_function(property, package, reader);
    } else if (node.kind == NodeKind::input && name == "on_submit") {
        node.on_submit = event_function(property, package, reader);
    } else if (name == "on_load") {
        if (!root || !is_container(node.kind)) {
            reader.fail_at(property.offset, "on_load is accepted only on a container page root");
        }
        node.on_load = event_function(property, package, reader);
    } else {
        reader.fail_at(property.offset, "property '" + std::string(name) +
                                            "' is not valid for this element kind");
    }
}

enum class FormatConversion : uint8_t { none, floating, integer, string };

FormatConversion validate_format(std::string_view format, ByteReader& reader, std::size_t offset) {
    FormatConversion result = FormatConversion::none;
    for (std::size_t position = 0; position < format.size();) {
        if (format[position] != '%') {
            ++position;
            continue;
        }
        if (position + 1 < format.size() && format[position + 1] == '%') {
            position += 2;
            continue;
        }
        if (result != FormatConversion::none) {
            reader.fail_at(offset, "format contains more than one conversion");
        }
        if (position + 1 >= format.size()) {
            reader.fail_at(offset, "format ends with an incomplete conversion");
        }
        if (format[position + 1] == 'd') {
            result = FormatConversion::integer;
            position += 2;
        } else if (format[position + 1] == 's') {
            result = FormatConversion::string;
            position += 2;
        } else if (format[position + 1] == 'f') {
            result = FormatConversion::floating;
            position += 2;
        } else if (format[position + 1] == '.') {
            if (position + 3 >= format.size() || format[position + 2] < '0' ||
                format[position + 2] > '6' || format[position + 3] != 'f') {
                reader.fail_at(offset, "format precision must be %.0f through %.6f");
            }
            result = FormatConversion::floating;
            position += 4;
        } else {
            reader.fail_at(offset, "format conversion must be %f, %.Nf, %d, or %s");
        }
    }
    return result;
}

void parse_samples(Node& node, const Property& property, ByteReader& reader) {
    require_property_kind(property, PropertyKind::string, reader);
    node.sample_count = 0;
    node.sample_head = 0;
    if (property.string.empty()) {
        return;
    }
    std::size_t begin = 0;
    while (begin <= property.string.size()) {
        const std::size_t comma = property.string.find(',', begin);
        const std::size_t end = comma == std::string_view::npos ? property.string.size() : comma;
        if (node.sample_count == max_chart_samples) {
            reader.fail_at(property.offset, "chart samples exceed 64 values");
        }
        const std::string_view token = property.string.substr(begin, end - begin);
        try {
            node.samples[node.sample_count] = parse_finite_float(token, "chart sample");
        } catch (const PackageError& error) {
            reader.fail_at(property.offset, error.what());
        }
        ++node.sample_count;
        if (comma == std::string_view::npos) {
            break;
        }
        begin = comma + 1;
    }
}

void apply_special_property(Node& node, const Property& property, ByteReader& reader) {
    if (node.kind == NodeKind::chart) {
        parse_samples(node, property, reader);
    } else {
        require_property_kind(property, PropertyKind::string, reader);
        node.source = property.string;
    }
}

void validate_node(const Node& node, const Package& package, const NodeFlags& flags,
                   std::size_t offset, ByteReader& reader) {
    if ((node.kind == NodeKind::input || node.kind == NodeKind::slider ||
         node.kind == NodeKind::progress) &&
        !flags.has_binding) {
        reader.fail_at(offset, "input, slider, and progress require a state binding");
    }
    if (node.kind == NodeKind::slider) {
        if (node.minimum > node.maximum || node.step <= 0.0F) {
            reader.fail_at(offset, "slider requires min <= max and step > 0");
        }
    }
    if (node.kind == NodeKind::progress && node.minimum >= node.maximum) {
        reader.fail_at(offset, "progress requires min < max");
    }
    if (node.kind == NodeKind::image && node.source.empty()) {
        reader.fail_at(offset, "image requires a nonempty src asset name");
    }
    if (flags.has_format) {
        if (!flags.has_binding || node.format.empty()) {
            reader.fail_at(offset, "a nonempty format requires a bound text element");
        }
        const FormatConversion conversion = validate_format(node.format, reader, offset);
        if (conversion == FormatConversion::none) {
            reader.fail_at(offset, "format must contain exactly one conversion");
        }
        const Value::Type state_type = package.states[node.binding].initial.type;
        if (state_type == Value::Type::number && conversion != FormatConversion::floating &&
            conversion != FormatConversion::integer) {
            reader.fail_at(offset, "numeric state binding requires a numeric format");
        }
        if (state_type == Value::Type::string && conversion != FormatConversion::string) {
            reader.fail_at(offset, "string state binding requires %s");
        }
        if (state_type == Value::Type::boolean) {
            reader.fail_at(offset, "boolean binding cannot have a format");
        }
    }
}

std::unique_ptr<Node> parse_node(ByteReader& reader, const Package& package, std::size_t depth,
                                 bool root, std::size_t& node_count,
                                 std::unordered_set<std::string>& page_ids) {
    const std::size_t node_offset = reader.position();
    if (depth > max_node_depth) {
        reader.fail("element nesting exceeds 32 levels");
    }
    if (node_count == max_nodes) {
        reader.fail("package exceeds 2,048 elements");
    }
    ++node_count;
    auto node = std::make_unique<Node>();
    const uint8_t raw_kind = reader.read_u8();
    if (raw_kind > static_cast<uint8_t>(NodeKind::stack)) {
        reader.fail_at(node_offset, "unknown node kind " + std::to_string(raw_kind));
    }
    node->kind = static_cast<NodeKind>(raw_kind);
    if (reader.read_u8() != 0) {
        reader.fail_at(node_offset + 1, "reserved node flags must be zero");
    }
    const uint16_t property_count = reader.read_u16();
    if (property_count > max_properties) {
        reader.fail("element property count exceeds 40");
    }
    NodeFlags flags;
    std::unordered_set<std::string> property_names;
    for (uint16_t index = 0; index < property_count; ++index) {
        Property property = read_property(reader, package);
        if (!property_names.insert(std::string(property.name)).second) {
            reader.fail_at(property.offset, "duplicate property '" + std::string(property.name) + "'");
        }
        if (apply_common_property(*node, property, reader)) {
            continue;
        }
        if ((node->kind == NodeKind::chart && property.name == "samples") ||
            (node->kind == NodeKind::image && property.name == "src")) {
            apply_special_property(*node, property, reader);
        } else {
            apply_content_property(*node, property, package, root, flags, reader);
        }
    }
    validate_node(*node, package, flags, node_offset, reader);
    if (!node->id.empty() && !page_ids.insert(node->id).second) {
        reader.fail_at(node_offset, "duplicate element ID '" + node->id + "' on page");
    }

    const uint32_t child_count = reader.read_u32();
    if (child_count > max_node_children) {
        reader.fail("element exceeds 64 children");
    }
    if (child_count > 0 && !is_container(node->kind)) {
        reader.fail("only view, row, column, and stack may have children");
    }
    node->children.reserve(child_count);
    for (uint32_t index = 0; index < child_count; ++index) {
        node->children.push_back(parse_node(reader, package, depth + 1, false, node_count, page_ids));
    }
    return node;
}

Page parse_page(ByteReader& reader, const Package& package, std::size_t& node_count) {
    if (node_count == max_nodes) {
        reader.fail("package exceeds 2,048 elements");
    }
    Page page;
    page.name = string_at(package, reader.read_u32(), "PAGE page name");
    std::unordered_set<std::string> page_ids;
    page.root = std::move(*parse_node(reader, package, 1, true, node_count, page_ids));
    reader.finish();
    return page;
}

Asset parse_asset(ByteReader& reader, const Package& package) {
    Asset asset;
    asset.name = string_at(package, reader.read_u32(), "DATA asset name");
    asset.width = reader.read_u16();
    asset.height = reader.read_u16();
    if (asset.width == 0 || asset.height == 0 || asset.width > max_asset_dimension ||
        asset.height > max_asset_dimension) {
        reader.fail("asset dimensions must be nonzero and at most 2,048");
    }
    const uint32_t data_size = reader.read_u32();
    const uint64_t expected_bytes = static_cast<uint64_t>(asset.width) * asset.height * 2U;
    if (data_size != expected_bytes || reader.remaining() != data_size) {
        reader.fail("asset pixel data size does not equal width * height * 2");
    }
    const std::size_t pixel_count = static_cast<std::size_t>(asset.width) * asset.height;
    asset.pixels.resize(pixel_count);
    for (uint16_t& pixel : asset.pixels) {
        pixel = reader.read_u16();
    }
    reader.finish();
    return asset;
}

void validate_image_sources(const Package& package,
                            const std::unordered_set<std::string>& asset_names) {
    for (const Page& page : package.pages) {
        std::vector<const Node*> remaining;
        remaining.push_back(&page.root);
        while (!remaining.empty()) {
            const Node* node = remaining.back();
            remaining.pop_back();
            if (node->kind == NodeKind::image && asset_names.find(node->source) == asset_names.end()) {
                throw PackageError("PAGE image on '" + page.name + "' references unknown asset '" +
                                   node->source + "'");
            }
            for (const auto& child : node->children) {
                remaining.push_back(child.get());
            }
        }
    }
}

constexpr auto crc_table = [] {
    std::array<uint32_t, 256> result{};
    for (uint32_t index = 0; index < result.size(); ++index) {
        uint32_t value = index;
        for (unsigned bit = 0; bit < 8; ++bit) {
            value = (value & 1U) != 0 ? (value >> 1U) ^ 0xedb88320U : value >> 1U;
        }
        result[index] = value;
    }
    return result;
}();

uint32_t package_crc32(const std::vector<uint8_t>& bytes) {
    uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const uint8_t byte = index >= 16 && index < 20 ? 0 : bytes[index];
        crc = (crc >> 8U) ^ crc_table[(crc ^ byte) & 0xffU];
    }
    return crc ^ 0xffffffffU;
}

} // namespace

Package parse_package(const std::vector<uint8_t>& bytes) {
    try {
    if (bytes.size() > max_package_bytes) {
        throw PackageError("package exceeds 16 MiB");
    }
    if (bytes.size() < 20) {
        throw PackageError("package is shorter than the 20-byte header");
    }
    ByteReader header(bytes, 0, bytes.size(), "package header");
    if (header.read_u8() != 'M' || header.read_u8() != 'B' || header.read_u8() != 'P' ||
        header.read_u8() != 'K') {
        throw PackageError("package magic is not MBPK");
    }
    if (header.read_u16() != 2) {
        throw PackageError("unsupported package major version");
    }
    if (header.read_u16() != 0) {
        throw PackageError("unsupported package minor version");
    }
    if (header.read_u32() != bytes.size()) {
        throw PackageError("header package size does not match the supplied byte count");
    }
    const uint32_t section_count = header.read_u32();
    if (section_count < 4) {
        throw PackageError("package requires STRS, STAT, CODE, and at least one PAGE section");
    }
    if (section_count > (bytes.size() - 20) / 8) {
        throw PackageError("section count cannot fit in the package");
    }
    const uint32_t expected_crc = header.read_u32();
    const uint32_t actual_crc = package_crc32(bytes);
    if (actual_crc != expected_crc) {
        throw PackageError("package CRC32 mismatch");
    }

    Package package;
    ByteReader sections(bytes, 20, bytes.size(), "package sections");
    bool have_strings = false;
    bool have_states = false;
    bool have_code = false;
    bool have_data = false;
    std::unordered_set<std::string> page_names;
    std::unordered_set<std::string> asset_names;
    std::size_t node_count = 0;
    for (uint32_t section_index = 0; section_index < section_count; ++section_index) {
        const std::size_t section_offset = sections.position();
        const std::span<const uint8_t> identifier_bytes = sections.read_bytes(4);
        const std::array<uint8_t, 4> identifier = {identifier_bytes[0], identifier_bytes[1],
                                                   identifier_bytes[2], identifier_bytes[3]};
        const std::string name(identifier.begin(), identifier.end());
        const uint32_t payload_size = sections.read_u32();
        ByteReader payload = sections.read_nested(payload_size, "section '" +
                                                                     section_description(identifier) +
                                                                     "'");
        if (name == "STRS") {
            if (have_strings || have_states || have_code || !package.pages.empty() || have_data) {
                throw PackageError("duplicate or out-of-order STRS section");
            }
            parse_strings(payload, package);
            have_strings = true;
        } else if (name == "STAT") {
            if (!have_strings || have_states || have_code || !package.pages.empty() || have_data) {
                throw PackageError("duplicate or out-of-order STAT section");
            }
            parse_states(payload, package);
            have_states = true;
        } else if (name == "CODE") {
            if (!have_strings || !have_states || have_code || !package.pages.empty() || have_data) {
                throw PackageError("duplicate or out-of-order CODE section");
            }
            parse_functions(payload, package);
            have_code = true;
        } else if (name == "PAGE") {
            if (!have_strings || !have_states || !have_code || have_data) {
                throw PackageError("out-of-order PAGE section");
            }
            if (package.pages.size() == max_pages) {
                throw PackageError("package exceeds 32 pages");
            }
            Page page = parse_page(payload, package, node_count);
            if (page.name.empty()) {
                throw PackageError("page name cannot be empty");
            }
            if (!page_names.insert(page.name).second) {
                throw PackageError("duplicate page name '" + page.name + "'");
            }
            package.pages.push_back(std::move(page));
        } else if (name == "DATA") {
            if (!have_strings || !have_states || !have_code || package.pages.empty()) {
                throw PackageError("out-of-order DATA section");
            }
            have_data = true;
            Asset asset = parse_asset(payload, package);
            if (asset.name.empty() || !asset_names.insert(asset.name).second) {
                throw PackageError("asset name is empty or duplicated: '" + asset.name + "'");
            }
            package.assets.push_back(std::move(asset));
        } else {
            throw PackageError("unknown section '" + section_description(identifier) + "' at byte " +
                               std::to_string(section_offset));
        }
    }
    if (!have_strings || !have_states || !have_code || package.pages.empty()) {
        throw PackageError("package is missing a required STRS, STAT, CODE, or PAGE section");
    }
    sections.finish();
    validate_image_sources(package, asset_names);
    return package;
    } catch (const std::bad_alloc&) {
        throw PackageError("insufficient memory to decode package");
    }
}

std::vector<uint8_t> read_package_file(const char* path) {
    if (path == nullptr) {
        throw PackageError("package path is null");
    }
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw PackageError("cannot open package file '" + std::string(path) + "'");
    }
    const std::streamoff end = file.tellg();
    if (end < 0) {
        throw PackageError("cannot determine package file size");
    }
    if (static_cast<uint64_t>(end) > max_package_bytes) {
        throw PackageError("package file exceeds 16 MiB");
    }
    std::vector<uint8_t> bytes;
    try {
        bytes.resize(static_cast<std::size_t>(end));
    } catch (const std::bad_alloc&) {
        throw PackageError("insufficient memory to read package file");
    }
    file.seekg(0, std::ios::beg);
    if (!file) {
        throw PackageError("cannot seek package file '" + std::string(path) + "'");
    }
    if (!bytes.empty()) {
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file || static_cast<std::size_t>(file.gcount()) != bytes.size()) {
            throw PackageError("cannot read complete package file '" + std::string(path) + "'");
        }
    }
    return bytes;
}

} // namespace mb
