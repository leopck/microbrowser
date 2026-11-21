#include "microbrowser/types.h"

#include "microbrowser/vm.h"

#include <algorithm>
#include <limits>

namespace mb {

bool Rect::contains(int px, int py) const {
    return !empty() && px >= x && py >= y && px < x + width && py < y + height;
}

Rect Rect::clipped(Rect other) const {
    if (empty() || other.empty()) return {};
    const int left = std::max(x, other.x);
    const int top = std::max(y, other.y);
    const auto right = std::min(int64_t(x) + width, int64_t(other.x) + other.width);
    const auto bottom = std::min(int64_t(y) + height, int64_t(other.y) + other.height);
    if (right <= left || bottom <= top) return {};
    return {left, top, int(right - left), int(bottom - top)};
}

Rect Rect::united(Rect other) const {
    if (empty()) return other;
    if (other.empty()) return *this;
    const int left = std::min(x, other.x);
    const int top = std::min(y, other.y);
    const auto right = std::max(int64_t(x) + width, int64_t(other.x) + other.width);
    const auto bottom = std::max(int64_t(y) + height, int64_t(other.y) + other.height);
    const auto result_width = right - left;
    const auto result_height = bottom - top;
    if (result_width > std::numeric_limits<int>::max() ||
        result_height > std::numeric_limits<int>::max()) {
        return {left, top, std::numeric_limits<int>::max(), std::numeric_limits<int>::max()};
    }
    return {left, top, int(result_width), int(result_height)};
}

Value Value::from_number(float value) {
    Value result;
    result.type = Type::number;
    result.number = value;
    return result;
}

Value Value::from_bool(bool value) {
    Value result;
    result.type = Type::boolean;
    result.boolean = value;
    return result;
}

Value Value::from_string(std::string value) {
    Value result;
    result.type = Type::string;
    result.string = std::move(value);
    return result;
}


Value Value::from_integer(std::int64_t value) {
    Value result;
    result.type = Type::integer;
    result.integer = value;
    return result;
}

Value Value::from_bytes(ByteBuffer value) {
    return from_bytes(std::make_shared<const ByteBuffer>(std::move(value)));
}

Value Value::from_bytes(ByteBufferPtr value) {
    if (value == nullptr) throw ScriptError("byte buffers cannot be null");
    Value result;
    result.type = Type::bytes;
    result.bytes = std::move(value);
    return result;
}

Value Value::from_handle(DeviceHandle value) {
    if (!value.is_empty() &&
        (value.capability == Capability::none || value.slot == 0 ||
         value.object_generation == 0 || value.owner_generation == 0)) {
        throw ScriptError("device handle has invalid identity");
    }
    Value result;
    result.type = Type::handle;
    result.handle = value;
    return result;
}
Value Value::from_void() {
    return Value{};
}

} // namespace mb
