#include "platform/desktop.h"
#include "microbrowser/device_identity.h"
#include "microbrowser/vm.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class ScopedState final {
public:
    ScopedState() {
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_directory = std::filesystem::temp_directory_path() /
            ("microbrowser-persistence-" + std::to_string(timestamp));
        if (!std::filesystem::create_directory(m_directory)) {
            throw std::runtime_error("temporary state directory already exists");
        }
        m_file = m_directory / "state";
        if (::setenv("MB_STATE_FILE", m_file.c_str(), 1) != 0) {
            throw std::runtime_error("cannot set test-state path");
        }
    }

    ~ScopedState() {
        ::unsetenv("MB_STATE_FILE");
        std::error_code ignored;
        std::filesystem::remove(m_file, ignored);
        std::filesystem::remove(m_directory, ignored);
    }

    ScopedState(const ScopedState&) = delete;
    ScopedState& operator=(const ScopedState&) = delete;

    [[nodiscard]] const std::filesystem::path& file_path() const { return m_file; }

private:
    std::filesystem::path m_directory;
    std::filesystem::path m_file;
};

mb::Value loaded(const mb::DesktopPlatform& platform, const std::string& name) {
    std::optional<mb::Value> value = platform.load_value(name);
    if (!value) throw std::runtime_error("persisted value was missing: " + name);
    return std::move(*value);
}

void test_exact_integer_reload(ScopedState& state) {
    {
        mb::DesktopPlatform platform;
        platform.save_value("above_f53", mb::Value::from_integer(9'007'199'254'740'993LL));
        platform.save_value("negative", mb::Value::from_integer(-9'007'199'254'740'993LL));
    }
    mb::DesktopPlatform platform;
    const mb::Value above = loaded(platform, "above_f53");
    const mb::Value negative = loaded(platform, "negative");
    require(above.type == mb::Value::Type::integer, "integer did not keep its type");
    require(above.integer == 9'007'199'254'740'993LL, "integer lost exact bits above 2^53");
    require(negative.integer == -9'007'199'254'740'993LL, "negative integer did not round-trip");
    require(std::filesystem::file_size(state.file_path()) > 0, "state file was not written");
}

void test_binary_reload() {
    const mb::ByteBuffer expected{0U, 255U, 0U, 1U, 127U, 128U, 10U, 13U};
    {
        mb::DesktopPlatform platform;
        platform.save_value("binary", mb::Value::from_bytes(expected));
        platform.save_value("empty", mb::Value::from_bytes(mb::ByteBuffer{}));
    }
    mb::DesktopPlatform platform;
    const mb::Value binary = loaded(platform, "binary");
    require(binary.type == mb::Value::Type::bytes, "binary data did not keep its type");
    require(binary.bytes && *binary.bytes == expected, "binary NUL or non-UTF-8 bytes changed");
    const mb::Value empty = loaded(platform, "empty");
    require(empty.bytes && empty.bytes->empty(), "empty binary buffer did not round-trip");
}

template <typename Action>
void expect_script_error(Action&& action, std::string_view text) {
    bool observed = false;
    try {
        std::forward<Action>(action)();
    } catch (const mb::ScriptError& error) {
        observed = std::string_view(error.what()).find(text) != std::string_view::npos;
    }
    require(observed, std::string("expected storage error containing: ").append(text).c_str());
}

void test_rejected_values() {
    mb::DesktopPlatform platform;
    expect_script_error([&] { platform.save_value("handle", mb::Value::from_handle()); },
                        "cannot persist a device handle");
    mb::Value null_buffer;
    null_buffer.type = mb::Value::Type::bytes;
    expect_script_error([&] { platform.save_value("null", null_buffer); },
                        "cannot persist a null byte buffer");
}

void test_malformed_record_rejection(const ScopedState& state) {
    {
        std::ofstream output(state.file_path(), std::ios::binary | std::ios::trunc);
        output << "i:7374617465:00\n";
        if (!output) throw std::runtime_error("cannot write malformed test state");
    }
    mb::DesktopPlatform platform;
    expect_script_error([&] { (void)platform.load_value("state"); },
                        "integer encoding must contain sixteen hex digits");
}

} // namespace

int main() {
    try {
        ScopedState state;
        test_exact_integer_reload(state);
        test_binary_reload();
        test_rejected_values();
        test_malformed_record_rejection(state);
        std::cout << "native persistence passed: exact i64, binary, handles, malformed records\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "native persistence test failed: " << error.what() << '\n';
        return 1;
    }
}
