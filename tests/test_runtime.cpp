#include "microbrowser/browser.h"
#include "microbrowser/canvas.h"
#include "microbrowser/json.h"
#include "../core/input.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition "\n"; ++failures; } } while (false)
#define REQUIRE(condition) do { if (!(condition)) throw std::runtime_error(std::string(#condition) + " at " + std::to_string(__LINE__)); } while (false)

void expect_package_error(const std::vector<uint8_t>& bytes) {
    try {
        auto package = mb::parse_package(bytes);
        (void)package;
        CHECK(false && "malformed package was accepted");
    } catch (const mb::PackageError&) {
        // Expected public exception, not an unchecked buffer access or sentinel.
    }
}

void expect_script_error(const std::function<void()>& action) {
    try {
        action();
        CHECK(false && "invalid script was accepted");
    } catch (const mb::ScriptError&) {
    }
}

void expect_invalid_argument(const std::function<void()>& action) {
    try {
        action();
        CHECK(false && "invalid numeric formatting was accepted");
    } catch (const std::invalid_argument&) {
    }
}

uint32_t read_u32(const std::vector<uint8_t>& bytes, std::size_t offset) {
    return uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8U) |
           (uint32_t(bytes[offset + 2]) << 16U) | (uint32_t(bytes[offset + 3]) << 24U);
}

void write_u32(std::vector<uint8_t>& bytes, std::size_t offset, uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) {
        bytes[offset + index] = uint8_t(value >> (index * 8U));
    }
}

std::size_t first_page_payload(const std::vector<uint8_t>& bytes) {
    const uint32_t section_count = read_u32(bytes, 12);
    std::size_t offset = 20;
    for (uint32_t index = 0; index < section_count; ++index) {
        const std::size_t payload_size = read_u32(bytes, offset + 4);
        if (bytes[offset] == 'P' && bytes[offset + 1] == 'A' && bytes[offset + 2] == 'G' &&
            bytes[offset + 3] == 'E') {
            return offset + 8;
        }
        offset += 8 + payload_size;
    }
    throw std::runtime_error("test package has no PAGE section");
}

uint32_t package_string_id(const mb::Package& package, std::string_view value) {
    const auto found = std::find(package.strings.begin(), package.strings.end(), value);
    REQUIRE(found != package.strings.end());
    return static_cast<uint32_t>(std::distance(package.strings.begin(), found));
}

void refresh_package_crc(std::vector<uint8_t>& bytes) {
    write_u32(bytes, 16, 0);
    uint32_t crc = 0xffffffffU;
    for (const uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xedb88320U : crc >> 1U;
        }
    }
    write_u32(bytes, 16, crc ^ 0xffffffffU);
}

void set_number_property(std::vector<uint8_t>& bytes, std::size_t offset, uint32_t name_id,
                         float value) {
    write_u32(bytes, offset, name_id);
    bytes[offset + 4] = 1;
    write_u32(bytes, offset + 5, std::bit_cast<uint32_t>(value));
    refresh_package_crc(bytes);
}

struct TestPlatform final : mb::Platform {
    uint64_t time = 100;
    std::vector<mb::HttpResult> completed;
    std::map<std::string, mb::Value> values;
    std::string url;
    std::string response = R"({"current":{"temperature":24.5}})";
    std::string raw_response = "from server";
    int status = 200;
    std::vector<std::string> logs;

    uint64_t now_ms() const override { return time; }
    void start_request(uint32_t id, std::string requested_url) override {
        url = std::move(requested_url);
        completed.push_back({id, status, url.ends_with("/data") ? response : raw_response, {}, {}});
        if (completed.back().error.empty()) {
            completed.back().body = response.find("current") != std::string::npos ? response : raw_response;
        }
    }
    bool poll_request(mb::HttpResult& result) override {
        if (completed.empty()) return false;
        result = std::move(completed.front());
        completed.erase(completed.begin());
        return true;
    }
    void start_content(uint32_t, mb::ContentRequest request) override {
        url = std::move(request.url);
    }
    bool poll_content(mb::HttpResult&) override { return false; }
    std::optional<mb::Value> load_value(const std::string& name) const override {
        const auto found = values.find(name);
        if (found == values.end()) return std::nullopt;
        return found->second;
    }
    void save_value(const std::string& name, const mb::Value& value) override { values[name] = value; }
    void log(std::string_view message) override { logs.emplace_back(message); }
    void advance(uint64_t milliseconds) { time += milliseconds; }
};

uint64_t fingerprint(const mb::Browser& browser) {
    uint64_t hash = 14695981039346656037ULL;
    const std::size_t count = std::size_t(browser.screen_size().width) * std::size_t(browser.screen_size().height);
    for (std::size_t index = 0; index < count; ++index) {
        hash ^= browser.pixels()[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::vector<uint16_t> pixel_snapshot(const mb::Browser& browser) {
    const std::size_t count = std::size_t(browser.screen_size().width) * std::size_t(browser.screen_size().height);
    return std::vector<uint16_t>(browser.pixels(), browser.pixels() + count);
}

void check_pixels(const mb::Browser& browser, const std::vector<uint16_t>& expected) {
    const std::size_t count = std::size_t(browser.screen_size().width) * std::size_t(browser.screen_size().height);
    std::size_t differences = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (browser.pixels()[index] == expected[index]) continue;
        if (differences < 8) {
            std::cerr << "pixel difference x=" << index % size_t(browser.screen_size().width)
                      << " y=" << index / size_t(browser.screen_size().width)
                      << " expected=" << expected[index] << " actual=" << browser.pixels()[index] << '\n';
        }
        ++differences;
    }
    CHECK(differences == 0);
}

void click(mb::Browser& browser, int x, int y) {
    browser.pointer_down(x, y);
    browser.pointer_up(x, y);
    REQUIRE(browser.tick());
}

void drag_to(mb::Browser& browser, int x, int from_y, int to_y) {
    browser.pointer_down(x, from_y);
    browser.pointer_move(x, to_y);
    browser.pointer_up(x, to_y);
}

void test_package_geometry(const std::vector<uint8_t>& bytes) {
    const auto package = mb::parse_package(bytes);
    const std::size_t page = first_page_payload(bytes);
    const std::size_t width_property = page + 8;
    const std::size_t height_property = width_property + 9;
    const uint32_t width_id = package_string_id(package, "width");
    const uint32_t height_id = package_string_id(package, "height");
    REQUIRE(bytes[page + 6] == 2 && bytes[page + 7] == 0);
    REQUIRE(read_u32(bytes, width_property) == width_id);
    REQUIRE(read_u32(bytes, height_property) == height_id);

    const auto reject_property = [&](std::size_t offset, uint32_t name_id, float value) {
        auto changed = bytes;
        set_number_property(changed, offset, name_id, value);
        expect_package_error(changed);
    };
    reject_property(width_property, width_id, 16'385.0F);
    reject_property(height_property, height_id, -1.0F);
    reject_property(width_property, package_string_id(package, "x"), -16'385.0F);
    reject_property(width_property, package_string_id(package, "y"), 16'385.0F);
    reject_property(width_property, package_string_id(package, "font-size"), 13.0F);
    reject_property(width_property, package_string_id(package, "font-weight"), 500.0F);

    auto zero_width = bytes;
    set_number_property(zero_width, width_property, width_id, 0.0F);
    const auto accepted = mb::parse_package(zero_width);
    CHECK(accepted.pages.front().root.style.width.kind == mb::LengthKind::fixed);
    CHECK(accepted.pages.front().root.style.width.value == 0.0F);
}

bool non_background_pixels(const mb::Browser& browser, mb::Rect area, uint16_t background) {
    const mb::Rect bounds = area.clipped({0, 0, browser.screen_size().width, browser.screen_size().height});
    for (int y = bounds.y; y < bounds.y + bounds.height; ++y) {
        for (int x = bounds.x; x < bounds.x + bounds.width; ++x) {
            if (browser.pixels()[std::size_t(y) * std::size_t(browser.screen_size().width) +
                                 std::size_t(x)] != background) return true;
        }
    }
    return false;
}

void test_grow_header_unwraps_title(mb::Browser& browser) {
    browser.dispatch("show_grow");
    REQUIRE(browser.tick());
    const uint16_t background = mb::to_rgb565(mb::palette::white);
    CHECK(!non_background_pixels(browser, {0, 0, 100, 26}, background));
    CHECK(non_background_pixels(browser, {0, 30, 100, 16}, background));
    browser.dispatch("go_back");
    REQUIRE(browser.tick());
    CHECK(browser.page_name() == "main");
}

void test_font_metrics_and_rounding() {
    CHECK(mb::Canvas::text_width("ii", 14) < mb::Canvas::text_width("WW", 14));
    CHECK(mb::Canvas::text_width("AV", 14) <
          mb::Canvas::text_width("A", 14) + mb::Canvas::text_width("V", 14));
    const int wrap_width = mb::Canvas::text_width("one", 14) + 1;
    CHECK(mb::Canvas::wrapped_text_size("one two", wrap_width, 14).height ==
          2 * mb::Canvas::text_height(14));

    constexpr int width = 40;
    constexpr int height = 32;
    std::array<uint16_t, width * height> pixels{};
    const uint16_t background = mb::to_rgb565(mb::palette::white);
    pixels.fill(background);
    mb::Canvas canvas(pixels.data(), {width, height});
    canvas.text(1, 2, "e", 14, mb::palette::ink, 255, {});
    bool has_text = false;
    bool has_antialiasing = false;
    const uint16_t ink = mb::to_rgb565(mb::palette::ink);
    for (const uint16_t pixel : pixels) {
        has_text = has_text || pixel != background;
        has_antialiasing = has_antialiasing || (pixel != background && pixel != ink);
    }
    CHECK(has_text);
    CHECK(has_antialiasing);

    pixels.fill(background);
    canvas.fill_rounded({4, 4, 24, 16}, 8, mb::palette::blue, 255, {});
    CHECK(pixels[4 * width + 4] == background);
    CHECK(pixels[8 * width + 16] == mb::to_rgb565(mb::palette::blue));
    canvas.stroke_rounded({4, 4, 24, 16}, 8, mb::palette::ink, 255, {});
    CHECK(pixels[4 * width + 4] == background);
    CHECK(pixels[4 * width + 16] == ink);
    CHECK(pixels[8 * width + 16] == mb::to_rgb565(mb::palette::blue));

    const mb::TextWindow first = mb::input_window("iiiiWWWW", 0, 24, 14, 400);
    CHECK(first.begin_byte == 0);
    CHECK(first.caret_pixels == 0);
    CHECK(!first.text.empty());
    const mb::TextWindow last = mb::input_window("WWWWiiii", 8, 32, 14, 400);
    CHECK(last.begin_byte > 0);
    CHECK(last.caret_pixels <= 32);
    CHECK(mb::caret_byte_at_x(last.text, 0, 14, 400) == 0);
    CHECK(mb::caret_byte_at_x("\xc2\xb0", 0, 14, 400) == 0);
    CHECK(mb::caret_byte_at_x("\xc2\xb0", 999, 14, 400) == 2);
}

void test_canvas_image_clip() {
    std::array<uint16_t, 16> pixels;
    pixels.fill(0x1234);
    const auto original = pixels;
    const mb::Asset asset{"white", 1, 1, {0xffff}};
    mb::Canvas canvas(pixels.data(), {4, 4});

    canvas.image({0, 0, 2, 2}, asset, 255, {3, 3, 1, 1});
    CHECK(pixels == original);

    canvas.image({0, 0, 2, 2}, asset, 255, {});
    auto expected = original;
    expected[0] = expected[1] = expected[4] = expected[5] = 0xffff;
    CHECK(pixels == expected);
}

void test_integer_formatting() {
    CHECK(mb::format_value(mb::Value::from_number(42.5F), "%d") == "43");
    for (const float extreme : {std::numeric_limits<float>::infinity(),
                                -std::numeric_limits<float>::infinity(),
                                std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::max(),
                                std::numeric_limits<float>::lowest()}) {
        expect_invalid_argument([extreme] {
            (void)mb::format_value(mb::Value::from_number(extreme), "%d");
        });
    }
}

uint16_t dashboard_pixel(const mb::Browser& browser, int x, int y) {
    return browser.pixels()[std::size_t(y) * std::size_t(browser.screen_size().width) + std::size_t(x)];
}

void check_dashboard_pixels(const mb::Browser& browser) {
    CHECK(dashboard_pixel(browser, 12, 68) == mb::to_rgb565(mb::Color::rgb(0xf1, 0xf4, 0xf9)));
    CHECK(dashboard_pixel(browser, 40, 74) == mb::to_rgb565(mb::palette::surface));
}


void test_dashboard_layout(mb::Browser& browser, TestPlatform& platform) {
    platform.advance(600);
    REQUIRE(browser.tick());
    check_dashboard_pixels(browser);

    browser.dispatch("open_details");
    platform.advance(250);
    REQUIRE(browser.tick());
    CHECK(browser.page_name() == "details");
    CHECK(dashboard_pixel(browser, 12, 68) == mb::to_rgb565(mb::Color::rgb(0xf1, 0xf4, 0xf9)));
    CHECK(dashboard_pixel(browser, 40, 74) == mb::to_rgb565(mb::palette::surface));

    browser.dispatch("go_back");
    platform.advance(600);
    REQUIRE(browser.tick());
    CHECK(browser.page_name() == "dashboard");
    check_dashboard_pixels(browser);
}

void test_settings_scroll(mb::Browser& browser, TestPlatform& platform) {
    browser.dispatch("open_settings");
    REQUIRE(browser.tick());
    browser.scroll(-256);
    REQUIRE(browser.tick());
    const std::size_t logs_before = platform.logs.size();
    click(browser, 240, 120);
    REQUIRE(platform.logs.size() == logs_before + 1);
    CHECK(platform.logs.back() == "dashboard local selftest passed");

    browser.dispatch("go_back");
    REQUIRE(browser.tick());
}

void test_scroll_drag(mb::Browser& browser, TestPlatform& platform) {
    browser.dispatch("open_settings");
    REQUIRE(browser.tick());
    for (int drag = 0; drag < 4; ++drag) {
        drag_to(browser, 400, 240, 40);
        REQUIRE(browser.tick());
    }
    const std::size_t logs_before = platform.logs.size();
    click(browser, 240, 120);
    REQUIRE(platform.logs.size() == logs_before + 1);
    CHECK(platform.logs.back() == "dashboard local selftest passed");

    browser.scroll(256);
    REQUIRE(browser.tick());
    const uint64_t scripts_before_drag = browser.metrics().scripts;
    drag_to(browser, 35, 33, 120);
    REQUIRE(browser.tick());
    CHECK(browser.page_name() == "settings");
    CHECK(browser.metrics().scripts == scripts_before_drag);

    browser.pointer_down(35, 33);
    browser.pointer_move(35, 34);
    browser.pointer_up(35, 34);
    REQUIRE(browser.page_name() == "dashboard");
    REQUIRE(browser.tick());
}

void test_slider_drag(mb::Browser& browser, TestPlatform& platform) {
    browser.pointer_down(220, 246);
    browser.pointer_move(325, 80);
    browser.pointer_up(325, 80);
    browser.dispatch("save_target");
    CHECK(platform.values.at("thermostat.target").number == 24.5F);
}

void test_ui(const char* package_path) {
    TestPlatform platform;
    mb::Browser browser(platform, {320, 240});
    const std::vector<uint8_t> bytes = mb::read_package_file(package_path);
    test_package_geometry(bytes);
    browser.load(bytes);
    REQUIRE(browser.tick());
    CHECK(browser.page_name() == "main");
    CHECK(browser.metrics().node_count == 15);
    const uint64_t main_pixels = fingerprint(browser);
    CHECK(main_pixels != 0);
    std::vector<uint8_t> corrupt = bytes;
    corrupt[corrupt.size() - 1] ^= 1U;
    expect_package_error(corrupt);
    corrupt = bytes;
    corrupt[8] ^= 1U;
    expect_package_error(corrupt);
    for (std::size_t size : {std::size_t{0}, std::size_t{16}, std::size_t{19}, std::size_t{23}}) {
        expect_package_error(std::vector<uint8_t>(bytes.begin(), bytes.begin() + std::min(size, bytes.size())));
    }

    click(browser, 40, 20);
    CHECK(fingerprint(browser) != main_pixels);
    const std::vector<uint16_t> target_pixels = pixel_snapshot(browser);
    for (const std::string& log : platform.logs) std::cerr << "runtime log: " << log << '\n';
    browser.dispatch("save_target");
    REQUIRE(platform.values.contains("target"));
    REQUIRE(platform.values.at("target").number == 25.0F);
    browser.dispatch("go_settings");
    CHECK(browser.page_name() == "settings");
    CHECK(browser.last_error().empty());
    browser.dispatch("go_back");
    CHECK(browser.page_name() == "main");
    CHECK(browser.last_error().empty());
    REQUIRE(browser.tick());
    check_pixels(browser, target_pixels);
    click(browser, 40, 20);
    browser.dispatch("checkpoint");
    REQUIRE(platform.values.at("checkpoint").number == 30.0F);
    browser.dispatch("load_target");
    browser.dispatch("save_target");
    REQUIRE(platform.values.at("target").number == 25.0F);
    REQUIRE(browser.tick());
    check_pixels(browser, target_pixels);
    REQUIRE(platform.values.at("target").number == 25.0F);

    browser.pointer_down(128, 82);
    browser.pointer_move(184, 82);
    browser.dispatch("save_target");
    REQUIRE(platform.values.at("target").number == 75.0F);
    browser.pointer_up(184, 82);
    REQUIRE(browser.tick());

    click(browser, 24, 115);
    browser.key(mb::Key::left);
    browser.text_input("d");
    browser.key(mb::Key::right);
    browser.text_input("°");
    browser.key(mb::Key::backspace);
    browser.key(mb::Key::enter);
    REQUIRE(browser.tick());
    test_grow_header_unwraps_title(browser);
    REQUIRE(platform.values.at("room_name").string == "abdc");

    browser.dispatch("start_timer");
    platform.advance(20);
    REQUIRE(browser.tick());
    browser.dispatch("push_chart");
    CHECK(fingerprint(browser) != main_pixels);
    browser.dispatch("fade");
    REQUIRE(browser.tick());
    platform.advance(20);
    REQUIRE(browser.tick());

    browser.dispatch("fetch_target");
    REQUIRE(browser.tick());
    CHECK(platform.url == "http://example.test/data");
    browser.dispatch("save_target");
    REQUIRE(platform.values.at("target").number == 24.5F);
    platform.status = 503;
    browser.dispatch("fetch_target");
    REQUIRE(browser.tick());
    CHECK(!browser.last_error().empty());
    browser.dispatch("save_target");
    REQUIRE(platform.values.at("target").number == 24.5F);
    platform.status = 200;

    click(browser, 150, 20);
    CHECK(browser.page_name() == "settings");
    const uint64_t settings_pixels = fingerprint(browser);
    browser.dispatch("go_back");
    CHECK(browser.page_name() == "main");
    REQUIRE(browser.tick());
    CHECK(fingerprint(browser) != settings_pixels);
    expect_script_error([&] { browser.dispatch("does_not_exist"); });

    browser.load(bytes);
    REQUIRE(browser.tick());
    CHECK(fingerprint(browser) == main_pixels);
    browser.resize({480, 320});
    REQUIRE(browser.tick());
    CHECK(browser.screen_size().width == 480 && browser.screen_size().height == 320);
}

struct VmHost final : mb::VmHost {
    std::vector<mb::Value> states{mb::Value::from_number(0)};
    mb::Value read_state(uint32_t index) const override { return states.at(index); }
    void write_state(uint32_t index, const mb::Value& value) override { states.at(index) = value; }
    mb::Value builtin(uint8_t, std::span<const mb::Value>) override { throw mb::ScriptError("no host builtin"); }
};

void test_vm_and_json() {
    VmHost host;
    mb::Package package;
    package.states.push_back({"result", mb::Value::from_number(0)});
    mb::Function constant;
    constant.name = "constant";
    constant.constants.push_back(mb::Value::from_number(42));
    constant.code = {1, 0, 0, 24};
    package.functions.push_back(std::move(constant));
    mb::Function setter;
    setter.name = "setter";
    setter.code = {22, 0, 0, 0, 4, 0, 0, 24};
    package.functions.push_back(std::move(setter));
    CHECK(mb::execute_script(package, 0, {}, host).number == 42);
    CHECK(mb::execute_script(package, 1, {}, host).type == mb::Value::Type::void_value);
    CHECK(host.states[0].number == 42);

    mb::Package loop_package;
    mb::Function loop;
    loop.name = "nonprogress";
    loop.code = {2, 1, 20, 0xfd, 0xff};
    loop_package.functions.push_back(std::move(loop));
    expect_script_error([&] { (void)mb::execute_script(loop_package, 0, {}, host); });

    const auto scalar = mb::read_json_scalar(R"({"items":[{"value":"\uD83D\uDE00"}]})", "items.0.value", mb::Value::Type::string);
    (void)scalar;
    CHECK(mb::read_json_scalar("[12.5]", "0", mb::Value::Type::number).number == 12.5F);
    expect_script_error([&] { (void)mb::read_json_scalar("{} trailing", "", mb::Value::Type::string); });
}

mb::Package integer_math_package(int64_t left, int64_t right, uint8_t operation) {
    mb::Package package;
    package.format_major = 3;
    mb::Function function;
    function.name = "integer_math";
    function.constants = {mb::Value::from_integer(left), mb::Value::from_integer(right)};
    function.code = {1, 0, 0, 1, 1, 0, operation, 24};
    package.functions.push_back(std::move(function));
    return package;
}

void test_integer_arithmetic() {
    VmHost host;
    const auto exact = integer_math_package(9007199254740993LL, 1, 8);
    CHECK(mb::execute_script(exact, 0, {}, host).integer == 9007199254740994LL);
    CHECK(mb::execute_script(integer_math_package(-4, 2, 11), 0, {}, host).integer == -2);
    for (const auto& invalid : {
             integer_math_package(INT64_MAX, 1, 8),
             integer_math_package(INT64_MIN, 1, 9),
             integer_math_package(INT64_MAX, 2, 10),
             integer_math_package(INT64_MIN, -1, 11),
         }) {
        expect_script_error([&] { (void)mb::execute_script(invalid, 0, {}, host); });
    }
    auto negation = integer_math_package(INT64_MIN, 0, 0);
    negation.functions[0].code = {1, 0, 0, 12, 24};
    expect_script_error([&] { (void)mb::execute_script(negation, 0, {}, host); });
    auto legacy = integer_math_package(1, 2, 8);
    legacy.format_major = 2;
    expect_script_error([&] { (void)mb::execute_script(legacy, 0, {}, host); });
}

void test_device_value_abi() {
    const auto integer = mb::Value::from_integer(std::numeric_limits<int64_t>::max());
    CHECK(integer.type == mb::Value::Type::integer);
    CHECK(integer.integer == std::numeric_limits<int64_t>::max());
    const auto bytes = mb::Value::from_bytes(mb::ByteBuffer{0, 0x80, 0xff});
    REQUIRE(bytes.bytes != nullptr);
    CHECK(bytes.bytes->at(0) == 0);
    CHECK(bytes.bytes->at(1) == 0x80);
    CHECK(bytes.bytes->at(2) == 0xff);
    expect_script_error([] { (void)mb::Value::from_bytes(mb::ByteBufferPtr{}); });
    const auto gpio = mb::Value::from_handle(mb::DeviceHandle{mb::Capability::gpio, 1, 2, 3});
    CHECK(gpio.type == mb::Value::Type::handle);
    CHECK((gpio.handle == mb::DeviceHandle{mb::Capability::gpio, 1, 2, 3}));
    expect_script_error([] {
        (void)mb::Value::from_handle(mb::DeviceHandle{mb::Capability::gpio, 0, 0, 0});
    });
    expect_script_error([] {
        (void)mb::Value::from_handle(mb::DeviceHandle{mb::Capability::none, 1, 2, 3});
    });
    CHECK(mb::Value::from_handle().handle.is_empty());
}

void test_framebuffer_initialization() {
    TestPlatform platform;
    mb::Browser browser(platform, {64, 64});
    const std::size_t pixels = std::size_t(browser.screen_size().width) *
        std::size_t(browser.screen_size().height);
    REQUIRE(browser.pixels() != nullptr);
    for (std::size_t index = 0; index < pixels; ++index) {
        CHECK(browser.pixels()[index] == mb::to_rgb565(mb::palette::background));
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: runtime_tests DASHBOARD.mbp UI_TEST.mbp\n";
        return 2;
    }
    try {
        test_framebuffer_initialization();
        TestPlatform dashboard_platform;
        mb::Browser dashboard(dashboard_platform, {480, 320});
        dashboard.load(mb::read_package_file(argv[1]));
        REQUIRE(dashboard.tick());
    CHECK(dashboard.metrics().node_count >= 10);
        test_dashboard_layout(dashboard, dashboard_platform);
        test_settings_scroll(dashboard, dashboard_platform);
        test_scroll_drag(dashboard, dashboard_platform);
        test_slider_drag(dashboard, dashboard_platform);
        CHECK(fingerprint(dashboard) != 0);
        CHECK(dashboard.last_error().empty());
        test_ui(argv[2]);
        test_vm_and_json();
        test_integer_arithmetic();
        test_device_value_abi();
        test_canvas_image_clip();
        test_font_metrics_and_rounding();
        test_integer_formatting();
    } catch (const std::exception& error) {
        std::cerr << "FATAL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " assertions failed\n";
        return 1;
    }
    std::cout << "runtime tests passed: package, VM, JSON, layout, paint, input, animation, navigation, storage, network\n";
    return 0;
}
