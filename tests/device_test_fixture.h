#pragma once

#include "microbrowser/browser.h"
#include "microbrowser/device_api.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace device_test {

inline void append_u8(std::vector<std::uint8_t>& output, std::uint8_t value) {
    output.push_back(value);
}

inline void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

inline void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned byte = 0; byte < 4; ++byte) {
        output.push_back(static_cast<std::uint8_t>(value >> (byte * 8U)));
    }
}

inline void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (unsigned byte = 0; byte < 8; ++byte) {
        output.push_back(static_cast<std::uint8_t>(value >> (byte * 8U)));
    }
}

inline void patch_u32(std::vector<std::uint8_t>& output, std::size_t offset, std::uint32_t value) {
    for (unsigned byte = 0; byte < 4; ++byte) {
        output[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8U));
    }
}

struct FunctionSpec {
    std::string name;
    std::uint16_t parameter_count = 0;
    std::uint16_t local_count = 0;
    std::vector<mb::Value> constants;
    std::vector<std::uint8_t> code;

    std::uint16_t constant(mb::Value value) {
        if (constants.size() == 256) throw std::runtime_error("fixture constant capacity reached");
        constants.push_back(std::move(value));
        return static_cast<std::uint16_t>(constants.size() - 1);
    }

    FunctionSpec& push(mb::Value value) {
        append_u8(code, 1);
        append_u16(code, constant(std::move(value)));
        return *this;
    }

    FunctionSpec& push_bool(bool value) {
        append_u8(code, 2);
        append_u8(code, value ? 1 : 0);
        return *this;
    }

    FunctionSpec& load_state(std::uint16_t index) {
        append_u8(code, 3);
        append_u16(code, index);
        return *this;
    }

    FunctionSpec& store_state(std::uint16_t index) {
        append_u8(code, 4);
        append_u16(code, index);
        return *this;
    }

    FunctionSpec& load_local(std::uint16_t index) {
        append_u8(code, 5);
        append_u16(code, index);
        return *this;
    }

    FunctionSpec& store_local(std::uint16_t index) {
        append_u8(code, 6);
        append_u16(code, index);
        return *this;
    }

    FunctionSpec& pop() {
        append_u8(code, 7);
        return *this;
    }

    FunctionSpec& add() {
        append_u8(code, 8);
        return *this;
    }

    FunctionSpec& equal() {
        append_u8(code, 13);
        return *this;
    }

    FunctionSpec& builtin(std::uint8_t id, std::uint8_t argc) {
        append_u8(code, 23);
        append_u8(code, id);
        append_u8(code, argc);
        return *this;
    }

    FunctionSpec& device(std::uint16_t operation, std::uint8_t argc) {
        append_u8(code, 25);
        append_u16(code, operation);
        append_u8(code, argc);
        return *this;
    }

    FunctionSpec& finish() {
        append_u8(code, 24);
        return *this;
    }
};

class StringTable {
public:
    std::uint32_t add(std::string_view value) {
        const auto found = std::find_if(m_values.begin(), m_values.end(), [&](const std::string& item) {
            return item == value;
        });
        if (found != m_values.end()) return static_cast<std::uint32_t>(found - m_values.begin());
        m_values.emplace_back(value);
        return static_cast<std::uint32_t>(m_values.size() - 1);
    }

    [[nodiscard]] std::uint32_t id(std::string_view value) const {
        const auto found = std::find_if(m_values.begin(), m_values.end(), [&](const std::string& item) {
            return item == value;
        });
        if (found == m_values.end()) throw std::runtime_error("fixture string was not interned");
        return static_cast<std::uint32_t>(found - m_values.begin());
    }

    [[nodiscard]] std::vector<std::uint8_t> encode() const {
        std::vector<std::uint8_t> payload;
        append_u32(payload, static_cast<std::uint32_t>(m_values.size()));
        std::uint32_t offset = 0;
        for (const std::string& value : m_values) {
            append_u32(payload, offset);
            offset += static_cast<std::uint32_t>(value.size() + 1);
        }
        for (const std::string& value : m_values) {
            payload.insert(payload.end(), value.begin(), value.end());
            append_u8(payload, 0);
        }
        return payload;
    }

private:
    std::vector<std::string> m_values;
};

struct CapabilityDeclaration {
    mb::CapabilityVersion version;
    bool required = true;
};

struct PackageSpec {
    std::uint16_t format_major = 3;
    std::string first_page_on_load;
    std::vector<CapabilityDeclaration> capabilities;
    std::vector<std::string> permissions;
    std::vector<mb::StateField> states;
    std::vector<FunctionSpec> functions;

    std::uint16_t state(std::string_view name, mb::Value initial) {
        states.push_back(mb::StateField{std::string(name), std::move(initial)});
        return static_cast<std::uint16_t>(states.size() - 1);
    }

    [[nodiscard]] std::uint16_t state(std::string_view name) const {
        const auto found = std::find_if(states.begin(), states.end(), [&](const mb::StateField& field) {
            return field.name == name;
        });
        if (found == states.end()) throw std::runtime_error("unknown fixture state " + std::string(name));
        return static_cast<std::uint16_t>(found - states.begin());
    }

    FunctionSpec& function(std::string name, std::uint16_t parameters = 0,
                           std::uint16_t locals = 0) {
        FunctionSpec function;
        function.name = std::move(name);
        function.parameter_count = parameters;
        function.local_count = std::max(parameters, locals);
        functions.push_back(std::move(function));
        return functions.back();
    }

    [[nodiscard]] std::vector<std::uint8_t> encode() const {
        if (format_major != 2 && format_major != 3) {
            throw std::runtime_error("fixture package major must be 2 or 3");
        }
        if (format_major == 2 && (!capabilities.empty() || !permissions.empty())) {
            throw std::runtime_error("legacy fixture cannot encode hardware declarations");
        }
        StringTable strings;
        strings.add("main");
        if (!first_page_on_load.empty()) strings.add("on_load");
        for (const auto& state : states) {
            strings.add(state.name);
            if (state.initial.type == mb::Value::Type::string) strings.add(state.initial.string);
        }
        for (const auto& function : functions) {
            strings.add(function.name);
            for (const auto& constant : function.constants) {
                if (constant.type == mb::Value::Type::string) strings.add(constant.string);
            }
        }

        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> sections;
        sections.emplace_back("STRS", strings.encode());
        std::vector<std::uint8_t> caps;
        append_u16(caps, static_cast<std::uint16_t>(capabilities.size()));
        for (const auto& declaration : capabilities) {
            append_u16(caps, static_cast<std::uint16_t>(declaration.version.capability));
            append_u16(caps, declaration.version.major);
            append_u16(caps, declaration.version.minor);
            append_u16(caps, declaration.required ? 1 : 0);
        }
        if (format_major == 3) sections.emplace_back("CAPS", std::move(caps));
        std::vector<std::uint8_t> permissions_payload;
        append_u16(permissions_payload, static_cast<std::uint16_t>(permissions.size()));
        for (const std::string& endpoint : permissions) {
            append_u16(permissions_payload, static_cast<std::uint16_t>(endpoint.size()));
            permissions_payload.insert(permissions_payload.end(), endpoint.begin(), endpoint.end());
        }
        if (format_major == 3) sections.emplace_back("PERM", std::move(permissions_payload));

        std::vector<std::uint8_t> stat;
        append_u32(stat, static_cast<std::uint32_t>(states.size()));
        for (const auto& field : states) {
            append_u32(stat, strings.id(field.name));
            encode_value(stat, field.initial, strings);
        }
        sections.emplace_back("STAT", std::move(stat));

        std::vector<std::uint8_t> code;
        append_u32(code, static_cast<std::uint32_t>(functions.size()));
        for (const auto& function : functions) {
            append_u32(code, strings.id(function.name));
            append_u16(code, function.parameter_count);
            append_u16(code, function.local_count);
            append_u32(code, static_cast<std::uint32_t>(function.code.size()));
            append_u32(code, static_cast<std::uint32_t>(function.constants.size()));
            for (const auto& constant : function.constants) encode_value(code, constant, strings);
            code.insert(code.end(), function.code.begin(), function.code.end());
        }
        sections.emplace_back("CODE", std::move(code));

        // One view: PAGE name, node kind/flags/properties, child count.
        std::vector<std::uint8_t> page;
        append_u32(page, strings.id("main"));
        append_u8(page, 0);
        append_u8(page, 0);
        append_u16(page, first_page_on_load.empty() ? 0 : 1);
        if (!first_page_on_load.empty()) {
            const auto handler = std::find_if(functions.begin(), functions.end(),
                [&](const FunctionSpec& function) { return function.name == first_page_on_load; });
            if (handler == functions.end() || handler->parameter_count != 0) {
                throw std::runtime_error("fixture on_load requires a zero-argument function");
            }
            append_u32(page, strings.id("on_load"));
            append_u8(page, 1);
            append_u32(page, std::bit_cast<std::uint32_t>(
                static_cast<float>(handler - functions.begin())));
        }
        append_u32(page, 0);
        sections.emplace_back("PAGE", std::move(page));

        std::vector<std::uint8_t> output{'M', 'B', 'P', 'K'};
        append_u16(output, format_major);
        append_u16(output, 0);
        append_u32(output, 0);
        append_u32(output, static_cast<std::uint32_t>(sections.size()));
        append_u32(output, 0);
        for (const auto& [name, payload] : sections) {
            output.insert(output.end(), name.begin(), name.end());
            append_u32(output, static_cast<std::uint32_t>(payload.size()));
            output.insert(output.end(), payload.begin(), payload.end());
        }
        patch_u32(output, 8, static_cast<std::uint32_t>(output.size()));
        std::uint32_t crc = 0xffffffffU;
        for (std::uint8_t byte : output) {
            crc ^= byte;
            for (unsigned bit = 0; bit < 8; ++bit) {
                crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xedb88320U : crc >> 1U;
            }
        }
        patch_u32(output, 16, crc ^ 0xffffffffU);
        return output;
    }

private:
    static void encode_value(std::vector<std::uint8_t>& output, const mb::Value& value,
                             const StringTable& strings) {
        switch (value.type) {
        case mb::Value::Type::number:
            append_u8(output, 0);
            append_u32(output, std::bit_cast<std::uint32_t>(value.number));
            return;
        case mb::Value::Type::boolean:
            append_u8(output, 1);
            append_u8(output, value.boolean ? 1 : 0);
            return;
        case mb::Value::Type::string:
            append_u8(output, 2);
            append_u32(output, strings.id(value.string));
            return;
        case mb::Value::Type::integer:
            append_u8(output, 3);
            append_u64(output, std::bit_cast<std::uint64_t>(value.integer));
            return;
        case mb::Value::Type::bytes:
            if (!value.bytes) throw std::runtime_error("fixture contains null bytes");
            append_u8(output, 4);
            append_u32(output, static_cast<std::uint32_t>(value.bytes->size()));
            output.insert(output.end(), value.bytes->begin(), value.bytes->end());
            return;
        case mb::Value::Type::handle:
            if (!value.handle.is_empty()) throw std::runtime_error("fixture handles must be empty");
            append_u8(output, 5);
            append_u16(output, 0);
            return;
        case mb::Value::Type::void_value:
            throw std::runtime_error("void is not a package constant or initial state");
        }
        throw std::runtime_error("unknown fixture value type");
    }
};

struct TestPlatform final : mb::Platform {
    std::uint64_t time_ms = 0;
    std::map<std::string, mb::Value> values;
    std::vector<std::string> logs;
    struct ContentCall {
        std::uint32_t request_id;
        mb::ContentRequest request;
    };
    std::vector<ContentCall> content_requests;
    std::vector<mb::HttpResult> completed_content;

    // Host-only content responses. These do not model a network or radio.
    void complete_next_content(mb::HttpResult result) {
        if (content_requests.empty()) throw std::runtime_error("no pending test content request");
        result.id = content_requests.front().request_id;
        content_requests.erase(content_requests.begin());
        completed_content.push_back(std::move(result));
    }

    [[nodiscard]] std::uint64_t now_ms() const override { return time_ms; }
    void start_request(std::uint32_t, std::string) override {}
    [[nodiscard]] bool poll_request(mb::HttpResult&) override { return false; }
    void start_content(std::uint32_t id, mb::ContentRequest request) override {
        if (content_requests.size() == 8) throw std::runtime_error("test content request queue is full");
        content_requests.push_back({id, std::move(request)});
    }
    [[nodiscard]] bool poll_content(mb::HttpResult& result) override {
        if (completed_content.empty()) return false;
        result = std::move(completed_content.front());
        completed_content.erase(completed_content.begin());
        return true;
    }
    [[nodiscard]] std::optional<mb::Value> load_value(const std::string& name) const override {
        const auto found = values.find(name);
        if (found == values.end()) return std::nullopt;
        return found->second;
    }
    void save_value(const std::string& name, const mb::Value& value) override { values[name] = value; }
    void log(std::string_view message) override { logs.emplace_back(message); }
};

class GpioModel final : public mb::PeripheralBackend {
public:
    GpioModel()
        : m_operations{mb::DeviceOperation::open, mb::DeviceOperation::configure,
                       mb::DeviceOperation::start, mb::DeviceOperation::stop,
                       mb::DeviceOperation::close, mb::DeviceOperation::read,
                       mb::DeviceOperation::write, mb::DeviceOperation::subscribe,
                       mb::DeviceOperation::unsubscribe, mb::DeviceOperation::cancel} {}

    [[nodiscard]] std::span<const mb::DeviceOperation> operations() const noexcept override {
        return m_operations;
    }

    mb::IoStatus start_operation(const mb::BackendOperation& operation,
                                 std::weak_ptr<mb::PeripheralCompletionSink> completions) override {
        if (!valid_request(operation)) return mb::IoStatus::invalid_argument;
        m_pending.push_back(Pending{operation, std::move(completions), false});
        if (operation.id == mb::DeviceOperation::subscribe) {
            m_subscription = operation;
            m_completions = m_pending.back().completions;
        }
        return mb::IoStatus::completed;
    }

    mb::IoStatus cancel_operation(const mb::BackendOperation& operation) override {
        const auto found = std::find_if(m_pending.begin(), m_pending.end(), [&](const Pending& pending) {
            return pending.operation.operation == operation.operation;
        });
        if (found == m_pending.end() || found->cancelled) return mb::IoStatus::invalid_argument;
        found->cancelled = true;
        return mb::IoStatus::completed;
    }

    void release_application(std::uint64_t owner_generation) override {
        m_released_generations.push_back(owner_generation);
    }

    void complete_next() { complete_at(0); }

    void complete_at(std::size_t index) {
        Pending pending = take(index);
        mb::IoStatus status = pending.cancelled ? mb::IoStatus::cancelled : mb::IoStatus::completed;
        mb::DeviceResponse response = response_for(pending.operation);
        deliver(pending, status, std::move(response));
    }

    void complete_next_with(mb::IoStatus status, mb::DeviceResponse response) {
        Pending pending = take(0);
        deliver(pending, status, std::move(response));
    }

    mb::IoStatus publish(bool high) {
        if (!m_subscription) throw std::runtime_error("GPIO model has no retained subscription");
        const auto sink = m_completions.lock();
        if (!sink) return mb::IoStatus::device_lost;
        return sink->publish(m_subscription->owner_generation, m_subscription->subscription,
                             mb::GpioReadResponse{high, m_next_timestamp_ns++});
    }

    [[nodiscard]] std::size_t pending_count() const { return m_pending.size(); }
    [[nodiscard]] std::size_t release_count() const { return m_released_generations.size(); }
    [[nodiscard]] const mb::BackendOperation& pending_operation(std::size_t index = 0) const {
        return m_pending.at(index).operation;
    }

    bool high = false;

private:
    struct Pending {
        mb::BackendOperation operation;
        std::weak_ptr<mb::PeripheralCompletionSink> completions;
        bool cancelled = false;
    };

    [[nodiscard]] static constexpr mb::CapabilityVersion version() {
        return {mb::Capability::gpio, mb::peripheral_abi_major, 0};
    }

    [[nodiscard]] static bool valid_request(const mb::BackendOperation& operation) {
        switch (operation.id) {
        case mb::DeviceOperation::open: return std::holds_alternative<mb::OpenRequest>(operation.request);
        case mb::DeviceOperation::configure: return std::holds_alternative<mb::GpioConfigureRequest>(operation.request);
        case mb::DeviceOperation::start: return std::holds_alternative<mb::StartRequest>(operation.request);
        case mb::DeviceOperation::stop: return std::holds_alternative<mb::StopRequest>(operation.request);
        case mb::DeviceOperation::close: return std::holds_alternative<mb::CloseRequest>(operation.request);
        case mb::DeviceOperation::read: return std::holds_alternative<mb::GpioReadRequest>(operation.request);
        case mb::DeviceOperation::write: return std::holds_alternative<mb::GpioWriteRequest>(operation.request);
        case mb::DeviceOperation::subscribe: return std::holds_alternative<mb::GpioSubscribeRequest>(operation.request);
        case mb::DeviceOperation::unsubscribe: return std::holds_alternative<mb::UnsubscribeRequest>(operation.request);
        default: return false;
        }
    }

    [[nodiscard]] mb::DeviceResponse response_for(const mb::BackendOperation& operation) {
        switch (operation.id) {
        case mb::DeviceOperation::open: return mb::OpenResponse{{}, version(), true};
        case mb::DeviceOperation::configure: {
            const auto& request = std::get<mb::GpioConfigureRequest>(operation.request);
            if (request.direction == mb::GpioDirection::output) high = request.initial_high;
            return mb::EmptyResponse{};
        }
        case mb::DeviceOperation::read:
            return mb::GpioReadResponse{high, m_next_timestamp_ns++};
        case mb::DeviceOperation::write:
            high = std::get<mb::GpioWriteRequest>(operation.request).high;
            return mb::EmptyResponse{};
        case mb::DeviceOperation::subscribe:
            return mb::SubscriptionResponse{operation.subscription};
        case mb::DeviceOperation::unsubscribe:
            // Retain the captured route so tests can verify SDK rejection after release.
            return mb::EmptyResponse{};
        default: return mb::EmptyResponse{};
        }
    }

    [[nodiscard]] Pending take(std::size_t index) {
        if (index >= m_pending.size()) throw std::runtime_error("GPIO model has no pending operation");
        Pending pending = std::move(m_pending[index]);
        m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(index));
        return pending;
    }

    static void deliver(const Pending& pending, mb::IoStatus status, mb::DeviceResponse response) {
        const auto sink = pending.completions.lock();
        if (sink) sink->complete(pending.operation, status, std::move(response));
    }

    std::vector<mb::DeviceOperation> m_operations;
    std::vector<Pending> m_pending;
    std::vector<std::uint64_t> m_released_generations;
    std::optional<mb::BackendOperation> m_subscription;
    std::weak_ptr<mb::PeripheralCompletionSink> m_completions;
    std::int64_t m_next_timestamp_ns = 100;
};

class UsbModel final : public mb::PeripheralBackend {
public:
    UsbModel()
        : m_operations{mb::DeviceOperation::open, mb::DeviceOperation::configure,
                       mb::DeviceOperation::transfer, mb::DeviceOperation::close} {}

    [[nodiscard]] std::span<const mb::DeviceOperation> operations() const noexcept override {
        return m_operations;
    }

    mb::IoStatus start_operation(const mb::BackendOperation& operation,
                                 std::weak_ptr<mb::PeripheralCompletionSink> completions) override {
        if (operation.id == mb::DeviceOperation::open &&
            !std::holds_alternative<mb::OpenRequest>(operation.request)) {
            return mb::IoStatus::invalid_argument;
        }
        if (operation.id == mb::DeviceOperation::configure) {
            const auto* request = std::get_if<mb::UsbDeviceConfigureRequest>(&operation.request);
            if (!request || !request->device_descriptor || request->device_descriptor->empty()) {
                return mb::IoStatus::invalid_argument;
            }
        } else if (operation.id == mb::DeviceOperation::transfer &&
                   !std::holds_alternative<mb::UsbTransferRequest>(operation.request)) {
            return mb::IoStatus::invalid_argument;
        }
        m_pending.push_back({operation, std::move(completions)});
        return mb::IoStatus::completed;
    }

    mb::IoStatus cancel_operation(const mb::BackendOperation&) override {
        return mb::IoStatus::unsupported;
    }
    void release_application(std::uint64_t) override {}

    void complete_next() {
        if (m_pending.empty()) throw std::runtime_error("USB model has no pending operation");
        const Pending pending = std::move(m_pending.front());
        m_pending.erase(m_pending.begin());
        mb::DeviceResponse response = mb::EmptyResponse{};
        if (pending.operation.id == mb::DeviceOperation::open) {
            response = mb::OpenResponse{
                {}, {mb::Capability::usb_device, mb::peripheral_abi_major, 0}, true};
        } else if (pending.operation.id == mb::DeviceOperation::configure) {
            response = mb::UsbConfigureResponse{1};
        } else if (pending.operation.id == mb::DeviceOperation::transfer) {
            const auto& request = std::get<mb::UsbTransferRequest>(pending.operation.request);
            response = mb::UsbTransferResponse{
                mb::make_immutable_bytes(std::span<const std::uint8_t>{}), request.receive_bytes};
        }
        const auto sink = pending.completions.lock();
        if (sink) sink->complete(pending.operation, mb::IoStatus::completed, std::move(response));
    }

    [[nodiscard]] std::size_t pending_count() const { return m_pending.size(); }
    [[nodiscard]] const mb::BackendOperation& pending_operation() const { return m_pending.at(0).operation; }

private:
    struct Pending {
        mb::BackendOperation operation;
        std::weak_ptr<mb::PeripheralCompletionSink> completions;
    };

    std::vector<mb::DeviceOperation> m_operations;
    std::vector<Pending> m_pending;
};

struct GpioFixture {
    GpioFixture() {
        registry.register_endpoint({"gpio", {mb::Capability::gpio, mb::peripheral_abi_major, 0}, true},
                                   mb::EndpointAccess::application, model);
        registry.register_endpoint({"other_gpio", {mb::Capability::gpio, mb::peripheral_abi_major, 0}, true},
                                   mb::EndpointAccess::application, model);
        registry.register_endpoint({"owner_gpio", {mb::Capability::gpio, mb::peripheral_abi_major, 0}, true},
                                   mb::EndpointAccess::owner_only, model);
        registry.register_endpoint({"usb_device", {mb::Capability::usb_device, mb::peripheral_abi_major, 0}, true},
                                   mb::EndpointAccess::application, usb);
    }

    std::shared_ptr<GpioModel> model = std::make_shared<GpioModel>();
    std::shared_ptr<UsbModel> usb = std::make_shared<UsbModel>();
    TestPlatform platform;
    mb::PeripheralRegistry registry;
};

inline PackageSpec gpio_package() {
    PackageSpec package;
    package.capabilities.push_back({{mb::Capability::gpio, mb::peripheral_abi_major, 0}, true});
    package.permissions.push_back("gpio");
    package.state("device", mb::Value::from_handle());
    package.state("subscription", mb::Value::from_handle());
    package.state("operation", mb::Value::from_handle());
    package.state("status", mb::Value::from_integer(-1));
    package.state("callback_count", mb::Value::from_integer(0));
    package.state("observed_bool", mb::Value::from_bool(false));
    package.state("observed_int", mb::Value::from_integer(0));
    package.state("observed_bytes", mb::Value::from_bytes(mb::ByteBuffer{}));
    package.state("native_bool", mb::Value::from_bool(false));
    return package;
}

inline void increment_callbacks(PackageSpec& package, FunctionSpec& function) {
    function.load_state(package.state("callback_count")).push(mb::Value::from_integer(1)).add().store_state(
        package.state("callback_count"));
}

inline FunctionSpec& add_callback(PackageSpec& package, std::string name,
                                  std::string_view primary_state = {}) {
    const auto existing = std::find_if(package.functions.begin(), package.functions.end(),
        [&](const FunctionSpec& function) { return function.name == name; });
    if (existing != package.functions.end()) {
        if (existing->parameter_count != 3 || existing->local_count < 3) {
            throw std::runtime_error("fixture callback name is already used with incompatible arity");
        }
        return *existing;
    }
    FunctionSpec& function = package.function(std::move(name), 3, 3);
    function.load_local(1).store_state(package.state("status"));
    function.load_local(0).store_state(package.state("operation"));
    if (!primary_state.empty()) function.load_local(2).store_state(package.state(primary_state));
    increment_callbacks(package, function);
    return function.finish();
}

inline void add_export_int(PackageSpec& package, std::string function_name,
                           std::string_view state_name, std::string key) {
    FunctionSpec& function = package.function(std::move(function_name));
    function.push(mb::Value::from_string(std::move(key)))
        .load_state(package.state(state_name))
        .device(static_cast<std::uint16_t>(mb::DeviceNativeOperation::number), 1)
        .builtin(13, 2)
        .pop()
        .finish();
}

inline void add_export_bool(PackageSpec& package, std::string function_name,
                            std::string_view state_name, std::string key) {
    FunctionSpec& function = package.function(std::move(function_name));
    function.push(mb::Value::from_string(std::move(key)))
        .load_state(package.state(state_name))
        .builtin(15, 2)
        .pop()
        .finish();
}

} // namespace device_test
