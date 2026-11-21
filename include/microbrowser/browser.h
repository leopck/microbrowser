#pragma once

#include "microbrowser/package.h"
#include "microbrowser/peripherals.h"
#include "microbrowser/platform.h"
#include "microbrowser/types.h"
#include "microbrowser/vm.h"

#include <array>
#include <span>
#include <string_view>

namespace mb {
struct ReaderDocument;

enum class Key : uint8_t { backspace, enter, left, right, home, end_key, escape, toggle_stats };

struct Metrics {
    uint64_t frames = 0;
    uint64_t scripts = 0;
    uint64_t flushed_pixels = 0;
    uint64_t layout_count = 0;
    uint64_t last_layout_us = 0;
    uint64_t last_render_us = 0;
    uint64_t package_bytes = 0;
    uint32_t node_count = 0;
    uint32_t active_requests = 0;
    uint32_t state_count = 0;
};

struct Damage {
    static constexpr std::size_t capacity = 32;
    std::array<Rect, capacity> regions{};
    std::size_t count = 0;
};

class Browser final : private VmHost {
public:
    Browser(Platform& platform, Size size, std::string gateway_url = "http://127.0.0.1:8765",
            PeripheralRegistry* peripherals = nullptr);
    ~Browser();
    void resize(Size size);
    void load(std::vector<uint8_t> bytes);
    void open_url(std::string url, bool remember = true);
    // Explicit consent to gateway-executed browser content; ordinary URLs use Reader.
    void open_interactive_url(std::string url);
    // Confirm the currently displayed native-package URL, as its Reader action does.
    // Throws ScriptError if that exact confirmation is not current.
    void confirm_native_package();
    void navigate_back();

    // tick is called by the application loop. true means damage() must be presented.
    bool tick();
    [[nodiscard]] const uint16_t* pixels() const { return m_pixels.data(); }
    [[nodiscard]] Size screen_size() const { return m_size; }
    [[nodiscard]] const Damage& damage() const { return m_frame_damage; }
    [[nodiscard]] std::string_view page_name() const;
    [[nodiscard]] std::string_view last_error() const { return m_error; }
    [[nodiscard]] std::string_view location() const;
    [[nodiscard]] std::string_view content_mode() const;
    [[nodiscard]] const Metrics& metrics() const { return m_metrics; }

    void pointer_down(int x, int y);
    void pointer_move(int x, int y);
    void pointer_up(int x, int y);
    void scroll(int delta);
    void key(Key key);
    void text_input(std::string_view utf8);
    // Used by headless interaction tests and embedded application integration.
    void dispatch(std::string_view function_name);

private:
    struct UrlState;
    struct Animation {
        enum class Property : uint8_t { none, scale, opacity, offset_y };
    };

    struct Timer {
        uint32_t id = 0;
        uint32_t function = 0;
        uint64_t due = 0;
        uint64_t interval = 0;
        bool active = false;
        bool repeating = false;
    };

    struct PendingRequest {
        uint32_t id = 0;
        uint32_t state = no_index;
        std::string json_path;
        Value::Type expected_type = Value::Type::string;
        bool json_request = false;
        bool active = false;
    };

    struct DeviceCallback {
        DeviceHandle operation;
        DeviceHandle subscription;
        std::uint32_t function;
    };

    using Navigation = NavigationSignal;
    std::size_t m_cursor_byte = 0;
    int m_pointer_x = -1;
    int m_pointer_y = -1;

    Platform& m_platform;
    Size m_size;
    Package m_package;
    std::vector<Value> m_state;
    std::vector<uint16_t> m_pixels;
    std::unique_ptr<UrlState> m_url;
    // Borrowed from the Platform. It must outlive the Browser's device sessions.
    PeripheralRegistry* m_peripherals = nullptr;
    std::unique_ptr<DeviceSession> m_devices;
    std::vector<DeviceCallback> m_device_callbacks;
    std::vector<DeviceCallback> m_device_subscriptions;
    std::vector<std::string_view> m_device_permission_names;
    const DeviceResult* m_active_device_result = nullptr;
    std::string m_storage_prefix;
    std::vector<Rect> m_pending_regions;
    Damage m_frame_damage{};
    Page* m_page = nullptr;
    Node* m_focus = nullptr;
    Node* m_pointer_target = nullptr;
    Node* m_scroll_target = nullptr;
    int m_scroll_start_y = 0;
    float m_scroll_origin = 0.0F;
    std::array<uint32_t, 16> m_history{};
    uint32_t m_history_size = 0;
    std::array<Timer, 16> m_timers{};
    std::array<PendingRequest, 8> m_requests{};
    Metrics m_metrics{};
    std::string m_error;
    uint64_t m_error_until = 0;
    uint64_t m_next_blink = 0;
    uint64_t m_last_paint = 0;
    uint64_t m_next_id = 1;
    uint32_t m_invocations = 0;
    bool m_layout_dirty = true;
    bool m_pointer_down = false;
    bool m_scroll_dragging = false;
    bool m_show_keyboard = false;
    bool m_cursor_visible = true;
    bool m_show_stats = false;
    bool m_full_repaint = true;

    Value read_state(uint32_t index) const override;
    void write_state(uint32_t index, const Value& value) override;
    Value builtin(uint8_t id, std::span<const Value> arguments) override;
    Value device_call(std::uint16_t operation, std::span<const Value> arguments) override;

    void invalidate(Rect area);
    void invalidate(const Node& node);
    void invalidate_layout();
    void consume_regions();
    void start_animations();
    void animate_node(Node& node, Animation::Property property, float target, uint64_t duration);
    bool advance_animations(uint64_t now);
    void advance_timers(uint64_t now);
    void poll_network();
    void perform_layout();
    void paint();
    Node* hit_test(int x, int y);
    void edit_value(Node& node, float x_normalized);
    void cancel_pointer_gesture();
    void apply_scroll_drag();
    void changed(Node& node);
    void open_page(std::string_view name, bool remember);
    void perform_remote_action(Node& node);
    void process_content_requests();
    void queue_visible_images();
    enum class NativePackageIntent : uint8_t { review, confirmed };
    void open_reader_url(const std::string& url, bool interactive, bool remember,
        NativePackageIntent package_intent = NativePackageIntent::review);
    void apply_package(const std::vector<uint8_t>& bytes);
    void clear_url_view(bool forget_history);
    void install_reader_page(ReaderDocument document, const std::string& address);
    void fail_url_load(const std::string& message);
    void receive_image(std::size_t index, HttpResult&& result);
    void enqueue_image(std::size_t index);
    void receive_transfer(HttpResult&& result);
    void open_page_index(uint32_t page, bool remember);
    void invoke(uint32_t function);
    void invoke(std::uint32_t function, std::span<const Value> arguments);
    DeviceSession& prepare_devices();
    void validate_required_devices();
    void close_devices();
    void process_devices();
    void handle_error(std::string_view message);
    Node* find_node(std::string_view id);
    uint32_t find_function(std::string_view name) const;
    uint32_t find_state(std::string_view name) const;

    friend struct Layout;
    friend struct Painter;
};

} // namespace mb
