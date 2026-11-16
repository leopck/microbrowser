#include "microbrowser/canvas.h"
#include "microbrowser/browser.h"
#include "esp_idf.h"

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_qemu_rgb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <inttypes.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern const uint8_t dashboard_mbp_start[] asm("_binary_dashboard_mbp_start");
extern const uint8_t dashboard_mbp_end[] asm("_binary_dashboard_mbp_end");

namespace {

constexpr char kLogTag[] = "microbrowser_app";
constexpr mb::Size kScreenSize{480, 320};

struct TouchPoint {
    int x;
    int y;
};
constexpr TouchPoint kScrollFrom{400, 240};
constexpr TouchPoint kScrollTo{400, 40};
constexpr TouchPoint kSelftestButtonCenter{240, 217};
constexpr TouchPoint kSelftestButtonPixel{40, 215};
constexpr std::size_t kMaximumCommandBytes = 96;
constexpr std::size_t kMaximumUartBytesPerPoll = 64;
constexpr uint32_t kApplicationPeriodMs = 10;

void check_esp(esp_err_t error, std::string_view operation) {
    if (error == ESP_OK) return;
    std::string message(operation);
    message += ": ";
    message += esp_err_to_name(error);
    throw std::runtime_error(message);
}

int print_length(std::string_view text) {
    if (text.size() > std::size_t(std::numeric_limits<int>::max())) throw std::runtime_error("text exceeds UART formatting limits");
    return int(text.size());
}

class QemuDisplay final {
public:
    QemuDisplay() {
        esp_lcd_rgb_qemu_config_t config = {
            .width = uint32_t(kScreenSize.width),
            .height = uint32_t(kScreenSize.height),
            .bpp = RGB_QEMU_BPP_16,
        };
        check_esp(esp_lcd_new_rgb_qemu(&config, &m_panel), "creating QEMU RGB panel");
        check_esp(esp_lcd_panel_reset(m_panel), "resetting QEMU RGB panel");
        check_esp(esp_lcd_panel_init(m_panel), "initializing QEMU RGB panel");
        void* framebuffer = nullptr;
        check_esp(esp_lcd_rgb_qemu_get_frame_buffer(m_panel, &framebuffer), "acquiring the QEMU framebuffer");
        m_frame_buffer = static_cast<uint16_t*>(framebuffer);
    }

    ~QemuDisplay() {
        if (m_panel != nullptr) {
            const esp_err_t error = esp_lcd_panel_del(m_panel);
            if (error != ESP_OK) ESP_LOGE(kLogTag, "deleting QEMU RGB panel: %s", esp_err_to_name(error));
        }
    }

    QemuDisplay(const QemuDisplay&) = delete;
    QemuDisplay& operator=(const QemuDisplay&) = delete;
    QemuDisplay(QemuDisplay&&) = delete;
    QemuDisplay& operator=(QemuDisplay&&) = delete;

    void present(const mb::Browser& browser) {
        const mb::Damage& damage = browser.damage();
        for (std::size_t index = 0; index < damage.count; ++index) present_region(browser, damage.regions[index]);
    }

private:
    void present_region(const mb::Browser& browser, mb::Rect region) {
        if (region.empty()) return;
        for (int row = 0; row < region.height; ++row) {
            const int y = region.y + row;
            const std::size_t pixel_offset = std::size_t(y) * std::size_t(kScreenSize.width) + std::size_t(region.x);
            const uint16_t* source = browser.pixels() + pixel_offset;
            uint16_t* destination = m_frame_buffer + pixel_offset;
            const int height = region.width == kScreenSize.width ? region.height : 1;
            std::memcpy(destination, source, std::size_t(region.width) * std::size_t(height) * sizeof(uint16_t));
            check_esp(esp_lcd_panel_draw_bitmap(m_panel, region.x, y, region.x + region.width, y + height, destination),
                "presenting QEMU framebuffer damage");
            if (region.width == kScreenSize.width) return;
        }
    }

    esp_lcd_panel_handle_t m_panel = nullptr;
    uint16_t* m_frame_buffer = nullptr; // Borrowed from the QEMU panel; not owned or freed by QemuDisplay.
};

std::size_t internal_heap_free() {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

std::size_t internal_heap_minimum_free() {
    return heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

std::size_t main_stack_minimum_free_bytes() {
    return uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
}

void print_page(const mb::Browser& browser) {
    const std::string_view page = browser.page_name();
    std::printf("PAGE name=%.*s error_present=%u\n", print_length(page), page.data(), browser.last_error().empty() ? 0U : 1U);
}

void print_stats(const mb::Browser& browser, const mb::EspIdfPlatform& platform) {
    const mb::Metrics& metrics = browser.metrics();
    const std::string_view page = browser.page_name();
    std::printf(
        "MICROBROWSER_STATS page=%.*s nodes=%" PRIu32 " frames=%" PRIu64
        " scripts=%" PRIu64 " layouts=%" PRIu64 " layout_us=%" PRIu64
        " render_us=%" PRIu64 " package_bytes=%" PRIu64 " active_requests=%" PRIu32
        " heap_free=%zu heap_min_free=%zu psram_total=%zu psram_free=%zu network=%s memory_source=emulated stack_min_free_bytes=%zu\n",
        print_length(page), page.data(), metrics.node_count, metrics.frames, metrics.scripts, metrics.layout_count,
        metrics.last_layout_us, metrics.last_render_us, metrics.package_bytes, metrics.active_requests,
        internal_heap_free(), internal_heap_minimum_free(), heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM), platform.network_online() ? "online" : "offline",
        main_stack_minimum_free_bytes());
}

void print_ready(const mb::Browser& browser, const mb::EspIdfPlatform& platform) {
    const mb::Metrics& metrics = browser.metrics();
    const std::string_view page = browser.page_name();
    std::printf(
        "MICROBROWSER_QEMU_READY page=%.*s nodes=%" PRIu32 " frames=%" PRIu64
        " heap_free=%zu heap_min_free=%zu psram_total=%zu psram_free=%zu network=%s memory_source=emulated stack_min_free_bytes=%zu\n",
        print_length(page), page.data(), metrics.node_count, metrics.frames, internal_heap_free(),
        internal_heap_minimum_free(), heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM), platform.network_online() ? "online" : "offline", main_stack_minimum_free_bytes());
    std::printf("UART commands: call NAME | page | stats | selftest (maximum %zu bytes per line)\n", kMaximumCommandBytes);
    std::fflush(stdout);
}

bool render_once(mb::Browser& browser, QemuDisplay& display) {
    if (!browser.tick()) return false;
    display.present(browser);
    return true;
}

void selftest_invoke(mb::Browser& browser, QemuDisplay& display, std::string_view function,
    std::string_view expected_page = {}) {
    browser.dispatch(function);
    if (!browser.last_error().empty()) {
        throw std::runtime_error(std::string(function) + ": " + std::string(browser.last_error()));
    }
    if (!expected_page.empty() && browser.page_name() != expected_page) {
        throw std::runtime_error(std::string(function) + ": expected page " + std::string(expected_page));
    }
    render_once(browser, display);
}

void selftest_drag_to_bottom(mb::Browser& browser, QemuDisplay& display) {
    browser.pointer_down(kScrollFrom.x, kScrollFrom.y);
    browser.pointer_move(kScrollTo.x, kScrollTo.y);
    browser.pointer_up(kScrollTo.x, kScrollTo.y);
    if (!render_once(browser, display)) throw std::runtime_error("drag scrolling did not paint a frame");

    const std::size_t pixel_index = std::size_t(kSelftestButtonPixel.y) * std::size_t(kScreenSize.width) +
        std::size_t(kSelftestButtonPixel.x);
    const uint16_t actual = browser.pixels()[pixel_index];
    if (actual != mb::to_rgb565(mb::palette::border)) {
        throw std::runtime_error("drag scrolling did not expose the self-test button");
    }

    const uint64_t scripts = browser.metrics().scripts;
    browser.pointer_down(kSelftestButtonCenter.x, kSelftestButtonCenter.y);
    browser.pointer_up(kSelftestButtonCenter.x, kSelftestButtonCenter.y);
    if (browser.metrics().scripts != scripts + 1) {
        throw std::runtime_error("the visible self-test button did not accept a click");
    }
    if (!render_once(browser, display)) throw std::runtime_error("the self-test button did not repaint");
}

void run_selftest(mb::Browser& browser, QemuDisplay& display) {
    try {
        if (browser.page_name() != "dashboard") throw std::runtime_error("selftest must start on the dashboard page");
        if (!browser.last_error().empty()) throw std::runtime_error(std::string("existing browser error: ") + std::string(browser.last_error()));
        const uint64_t initial_frames = browser.metrics().frames;

        // Uses only existing dashboard functions with local effects: stop_timers,
        // open_details/go_back, refresh_sensors, open_settings, and NVS-only save_all.
        selftest_invoke(browser, display, "stop_timers");
        selftest_invoke(browser, display, "open_details", "details");
        selftest_invoke(browser, display, "refresh_sensors");
        selftest_invoke(browser, display, "go_back", "dashboard");
        selftest_invoke(browser, display, "open_settings", "settings");
        selftest_drag_to_bottom(browser, display);
        selftest_invoke(browser, display, "save_all");
        selftest_invoke(browser, display, "go_back", "dashboard");
        selftest_invoke(browser, display, "stop_timers");

        if (browser.metrics().frames <= initial_frames) throw std::runtime_error("selftest did not paint a new frame");
        const std::string_view page = browser.page_name();
        std::printf("SELFTEST PASS page=%.*s frames=%" PRIu64 " heap=%zu\n", print_length(page), page.data(),
            browser.metrics().frames, internal_heap_free());
    } catch (const std::exception& error) {
        std::printf("SELFTEST FAIL reason=%s\n", error.what());
    }
}

class UartCommands final {
public:
    UartCommands(mb::Browser& browser, QemuDisplay& display, mb::EspIdfPlatform& platform)
        : m_browser(browser), m_display(display), m_platform(platform) {
        if (!uart_is_driver_installed(UART_NUM_0)) {
            check_esp(uart_driver_install(UART_NUM_0, 256, 0, 0, nullptr, 0), "installing UART0 command input");
        }
    }

    void poll() {
        std::size_t available = 0;
        check_esp(uart_get_buffered_data_len(UART_NUM_0, &available), "polling UART0 command input");
        const std::size_t length = std::min(available, kMaximumUartBytesPerPoll);
        if (length == 0) return;
        std::array<uint8_t, kMaximumUartBytesPerPoll> bytes{};
        const int received = uart_read_bytes(UART_NUM_0, bytes.data(), uint32_t(length), 0);
        if (received < 0) throw std::runtime_error("reading UART0 command input failed");
        for (int index = 0; index < received; ++index) consume(bytes[std::size_t(index)]);
    }

private:
    void consume(uint8_t byte) {
        if (m_skip_next_lf) {
            m_skip_next_lf = false;
            if (byte == '\n') return;
        }
        if (byte == '\r' || byte == '\n') {
            m_skip_next_lf = byte == '\r';
            if (m_discarding) {
                m_discarding = false;
                m_length = 0;
                return;
            }
            execute(std::string(m_line.data(), m_length));
            std::fflush(stdout);
            m_length = 0;
            return;
        }
        if (m_discarding) return;
        if (m_length == kMaximumCommandBytes) {
            m_discarding = true;
            m_length = 0;
            std::printf("COMMAND FAIL reason=line exceeds %zu bytes\n", kMaximumCommandBytes);
            std::fflush(stdout);
            return;
        }
        m_line[m_length++] = char(byte);
    }

    void execute(std::string line) {
        if (line == "page") {
            print_page(m_browser);
            return;
        }
        if (line == "stats") {
            print_stats(m_browser, m_platform);
            return;
        }
        if (line == "selftest") {
            run_selftest(m_browser, m_display);
            return;
        }
        constexpr std::string_view call_prefix = "call ";
        if (line.starts_with(call_prefix) && line.size() > call_prefix.size()) {
            const std::string_view function(line.data() + call_prefix.size(), line.size() - call_prefix.size());
            try {
                m_browser.dispatch(function);
                if (!m_browser.last_error().empty()) throw std::runtime_error(std::string(m_browser.last_error()));
                render_once(m_browser, m_display);
                std::printf("COMMAND OK call=%.*s\n", print_length(function), function.data());
            } catch (const std::exception& error) {
                std::printf("COMMAND FAIL call=%.*s reason=%s\n", print_length(function), function.data(), error.what());
            }
            return;
        }
        if (line.empty()) {
            std::printf("COMMAND FAIL reason=empty command\n");
        } else if (line == "call " || line == "call") {
            std::printf("COMMAND FAIL reason=missing function name\n");
        } else {
            std::printf("COMMAND FAIL reason=unknown command\n");
        }
    }

    mb::Browser& m_browser;
    QemuDisplay& m_display;
    mb::EspIdfPlatform& m_platform;
    std::array<char, kMaximumCommandBytes> m_line{};
    std::size_t m_length = 0;
    bool m_discarding = false;
    bool m_skip_next_lf = false;
};

std::vector<uint8_t> packaged_dashboard() {
    const auto begin = reinterpret_cast<std::uintptr_t>(dashboard_mbp_start);
    const auto end = reinterpret_cast<std::uintptr_t>(dashboard_mbp_end);
    if (end < begin) throw std::runtime_error("embedded dashboard package has invalid bounds");
    if (end - begin > 16U * 1024U * 1024U) throw std::runtime_error("embedded dashboard exceeds the package limit");
    return std::vector<uint8_t>(dashboard_mbp_start, dashboard_mbp_end);
}

[[noreturn]] void run() {
    ESP_LOGI(kLogTag, "initializing platform");
    mb::EspIdfPlatform platform;
    QemuDisplay display;
    ESP_LOGI(kLogTag, "initializing the RGB565 framebuffer");
    mb::Browser browser(platform, kScreenSize);
    browser.load(packaged_dashboard());
    ESP_LOGI(kLogTag, "starting virtual Ethernet");
    platform.start_network();
    UartCommands commands(browser, display, platform);

    if (!render_once(browser, display)) throw std::runtime_error("initial dashboard did not produce a framebuffer");
    print_ready(browser, platform);

    TickType_t previous_wake = xTaskGetTickCount();
    for (;;) {
        commands.poll();
        render_once(browser, display);
        vTaskDelayUntil(&previous_wake, pdMS_TO_TICKS(kApplicationPeriodMs));
    }
}

} // namespace

extern "C" void app_main() {
    ESP_LOGI(kLogTag, "app_main entered");
    try {
        run();
    } catch (const std::exception& error) {
        ESP_LOGE(kLogTag, "fatal Microbrowser startup or runtime error: %s", error.what());
        std::abort();
    }
}
