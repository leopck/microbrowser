#include "desktop.h"
#include "event_replay.h"

#include "microbrowser/browser.h"
#include "microbrowser/canvas.h"
#include "microbrowser/package.h"

#if __has_include(<SDL.h>)
#include <SDL.h>
#else
#include <SDL2/SDL.h>
#endif

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace mb {
namespace {

constexpr Size default_size{480, 320};
constexpr int default_scale = 2;
constexpr uint64_t default_headless_frames = 120;
constexpr auto frame_interval = std::chrono::milliseconds(16);

class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Options {
    std::filesystem::path package;
    Size size = default_size;
    int scale = default_scale;
    std::optional<uint64_t> frame_limit;
    std::filesystem::path screenshot;
    bool has_screenshot = false;
    bool headless = false;
    std::filesystem::path events;
    bool has_events = false;
    std::optional<uint64_t> spi_hz;
    bool help = false;
};

void print_usage(std::ostream& output) {
    output <<
        "Usage: microbrowser --package FILE.mbp [--size 480x320] [--scale 2]\n"
        "                    [--frames N] [--screenshot FILE.ppm] [--headless]\n"
        "                    [--events FILE] [--spi-hz HZ]\n"
        "\n"
        "Runs the native SDL2 RGB565 simulator. Headless mode defaults to 120\n"
        "application frames; without --frames, windowed mode runs until close.\n"
        "Replay commands: click/down/move/up X Y, wait MS, key NAME, text TEXT,\n"
        "call FUNCTION, scroll DELTA, and shot FILE.ppm.\n";
}

template <typename T>
T parse_integer(std::string_view text, std::string_view option) {
    T value{};
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != end) {
        throw UsageError(std::string(option) + " requires a valid integer, got '" +
                         std::string(text) + "'");
    }
    return value;
}

std::string_view require_value(int argc, char* argv[], int& index) {
    if (index + 1 >= argc) throw UsageError(std::string(argv[index]) + " requires a value");
    ++index;
    return argv[index];
}

std::filesystem::path parse_path(std::string_view value, std::string_view option) {
    if (value.empty()) throw UsageError(std::string(option) + " requires a nonempty path");
    return std::filesystem::path(value);
}

Size parse_size(std::string_view text) {
    const std::size_t separator = text.find('x');
    if (separator == std::string_view::npos || text.find('x', separator + 1) != std::string_view::npos) {
        throw UsageError("--size requires WIDTHxHEIGHT, for example 480x320");
    }
    const int width = parse_integer<int>(text.substr(0, separator), "--size width");
    const int height = parse_integer<int>(text.substr(separator + 1), "--size height");
    if (width < 64 || width > 2048 || height < 64 || height > 2048) {
        throw UsageError("framebuffer width and height must both be in the range 64..2048");
    }
    const auto unsigned_width = static_cast<std::size_t>(width);
    const auto unsigned_height = static_cast<std::size_t>(height);
    if (unsigned_width > std::numeric_limits<std::size_t>::max() / unsigned_height ||
        unsigned_width * unsigned_height > std::numeric_limits<std::size_t>::max() / 2) {
        throw UsageError("framebuffer byte size overflows this host's size type");
    }
    return Size{width, height};
}

Options parse_options(int argc, char* argv[]) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--package") {
            options.package = parse_path(require_value(argc, argv, index), argument);
        } else if (argument == "--size") {
            options.size = parse_size(require_value(argc, argv, index));
        } else if (argument == "--scale") {
            options.scale = parse_integer<int>(require_value(argc, argv, index), argument);
            if (options.scale < 1 || options.scale > 4) {
                throw UsageError("--scale must be in the range 1..4");
            }
        } else if (argument == "--frames") {
            const uint64_t frames = parse_integer<uint64_t>(require_value(argc, argv, index), argument);
            if (frames == 0) throw UsageError("--frames must be at least one");
            options.frame_limit = frames;
        } else if (argument == "--screenshot") {
            options.screenshot = parse_path(require_value(argc, argv, index), argument);
            options.has_screenshot = true;
        } else if (argument == "--headless") {
            options.headless = true;
        } else if (argument == "--events") {
            options.events = parse_path(require_value(argc, argv, index), argument);
            options.has_events = true;
        } else if (argument == "--spi-hz") {
            const uint64_t frequency = parse_integer<uint64_t>(require_value(argc, argv, index), argument);
            if (frequency == 0) throw UsageError("--spi-hz must be greater than zero");
            options.spi_hz = frequency;
        } else {
            throw UsageError("unknown argument '" + std::string(argument) + "'");
        }
    }
    if (!options.help && options.package.empty()) throw UsageError("--package is required");
    return options;
}

std::runtime_error sdl_error(std::string_view operation) {
    return std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}

class SdlContext {
public:
    SdlContext() {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
            throw sdl_error("SDL initialization failed");
        }
        SDL_StartTextInput();
    }

    ~SdlContext() {
        SDL_StopTextInput();
        SDL_Quit();
    }

    SdlContext(const SdlContext&) = delete;
    SdlContext& operator=(const SdlContext&) = delete;
};

struct WindowDeleter {
    void operator()(SDL_Window* window) const {
        if (window != nullptr) SDL_DestroyWindow(window);
    }
};
struct RendererDeleter {
    void operator()(SDL_Renderer* renderer) const {
        if (renderer != nullptr) SDL_DestroyRenderer(renderer);
    }
};
struct TextureDeleter {
    void operator()(SDL_Texture* texture) const {
        if (texture != nullptr) SDL_DestroyTexture(texture);
    }
};

struct LogicalPoint {
    int x = 0;
    int y = 0;
};

SDL_Rect fitted_viewport(Size logical, int output_width, int output_height) {
    if (output_width <= 0 || output_height <= 0) return SDL_Rect{0, 0, 0, 0};
    SDL_Rect result{0, 0, output_width, output_height};
    const int64_t width_ratio = static_cast<int64_t>(output_width) * logical.height;
    const int64_t height_ratio = static_cast<int64_t>(output_height) * logical.width;
    if (width_ratio <= height_ratio) {
        result.h = std::max(1, static_cast<int>(width_ratio / logical.width));
        result.y = (output_height - result.h) / 2;
    } else {
        result.w = std::max(1, static_cast<int>(height_ratio / logical.height));
        result.x = (output_width - result.w) / 2;
    }
    return result;
}

class SdlDisplay {
public:
    SdlDisplay(Size logical_size, int scale) : m_logical_size(logical_size) {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
        m_window.reset(SDL_CreateWindow(
            "Microbrowser", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            logical_size.width * scale, logical_size.height * scale,
            SDL_WINDOW_RESIZABLE));
        if (!m_window) throw sdl_error("SDL window creation failed");
        m_renderer.reset(SDL_CreateRenderer(m_window.get(), -1, SDL_RENDERER_SOFTWARE));
        if (!m_renderer) throw sdl_error("SDL software renderer creation failed");
        m_texture.reset(SDL_CreateTexture(m_renderer.get(), SDL_PIXELFORMAT_RGB565,
                                          SDL_TEXTUREACCESS_STREAMING,
                                          logical_size.width, logical_size.height));
        if (!m_texture) throw sdl_error("SDL RGB565 texture creation failed");
        if (SDL_SetTextureBlendMode(m_texture.get(), SDL_BLENDMODE_NONE) != 0) {
            throw sdl_error("SDL texture configuration failed");
        }
    }

    uint64_t present(const uint16_t* pixels, const Damage* damage) {
        uint64_t uploaded_pixels = 0;
        if (damage != nullptr) {
            const Rect screen{0, 0, m_logical_size.width, m_logical_size.height};
            for (std::size_t index = 0; index < damage->count; ++index) {
                const Rect region = damage->regions[index].clipped(screen);
                if (region.empty()) continue;
                const SDL_Rect source{region.x, region.y, region.width, region.height};
                const uint16_t* const start = pixels + region.y * m_logical_size.width + region.x;
                if (SDL_UpdateTexture(m_texture.get(), &source, start,
                                      m_logical_size.width * static_cast<int>(sizeof(uint16_t))) != 0) {
                    throw sdl_error("SDL RGB565 damage upload failed");
                }
                uploaded_pixels += static_cast<uint64_t>(region.width) *
                    static_cast<uint64_t>(region.height);
            }
        }

        // SDL back buffers are undefined after Present. Keep the complete image in
        // m_texture while uploading only damage, then redraw from that cache.
        int output_width = 0;
        int output_height = 0;
        if (SDL_GetRendererOutputSize(m_renderer.get(), &output_width, &output_height) != 0) {
            throw sdl_error("SDL output-size query failed");
        }
        if (output_width > 0 && output_height > 0) {
            const SDL_Rect destination = fitted_viewport(m_logical_size, output_width, output_height);
            if (SDL_SetRenderDrawColor(m_renderer.get(), 0, 0, 0, SDL_ALPHA_OPAQUE) != 0 ||
                SDL_RenderClear(m_renderer.get()) != 0 ||
                SDL_RenderCopy(m_renderer.get(), m_texture.get(), nullptr, &destination) != 0) {
                throw sdl_error("SDL framebuffer presentation failed");
            }
            SDL_RenderPresent(m_renderer.get());
        }
        m_needs_present = false;
        return uploaded_pixels;
    }

    [[nodiscard]] bool needs_present() const { return m_needs_present; }
    void request_present() { m_needs_present = true; }

    [[nodiscard]] std::optional<LogicalPoint> map_point(int x, int y, bool clamp_to_screen) const {
        int window_width = 0;
        int window_height = 0;
        SDL_GetWindowSize(m_window.get(), &window_width, &window_height);
        const SDL_Rect viewport = fitted_viewport(m_logical_size, window_width, window_height);
        if (viewport.w <= 0 || viewport.h <= 0) return std::nullopt;
        if (!clamp_to_screen &&
            (x < viewport.x || x >= viewport.x + viewport.w ||
             y < viewport.y || y >= viewport.y + viewport.h)) {
            return std::nullopt;
        }
        const int local_x = std::clamp(x, viewport.x, viewport.x + viewport.w - 1) - viewport.x;
        const int local_y = std::clamp(y, viewport.y, viewport.y + viewport.h - 1) - viewport.y;
        const int logical_x = static_cast<int>(static_cast<int64_t>(local_x) * m_logical_size.width / viewport.w);
        const int logical_y = static_cast<int>(static_cast<int64_t>(local_y) * m_logical_size.height / viewport.h);
        return LogicalPoint{std::min(logical_x, m_logical_size.width - 1),
                            std::min(logical_y, m_logical_size.height - 1)};
    }

private:
    Size m_logical_size;
    std::unique_ptr<SDL_Window, WindowDeleter> m_window{nullptr};
    std::unique_ptr<SDL_Renderer, RendererDeleter> m_renderer{nullptr};
    std::unique_ptr<SDL_Texture, TextureDeleter> m_texture{nullptr};
    bool m_needs_present = true;
};

std::optional<Key> translate_key(SDL_Keycode key) {
    switch (key) {
    case SDLK_BACKSPACE: return Key::backspace;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return Key::enter;
    case SDLK_LEFT: return Key::left;
    case SDLK_RIGHT: return Key::right;
    case SDLK_HOME: return Key::home;
    case SDLK_END: return Key::end_key;
    case SDLK_ESCAPE: return Key::escape;
    case SDLK_F1: return Key::toggle_stats;
    default: return std::nullopt;
    }
}

bool process_sdl_events(Browser& browser, SdlDisplay& display, bool& pointer_is_down) {
    SDL_Event event;
    while (SDL_PollEvent(&event) != 0) {
        switch (event.type) {
        case SDL_QUIT:
            return false;
        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_CLOSE) return false;
            if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                event.window.event == SDL_WINDOWEVENT_EXPOSED) {
                display.request_present();
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (event.button.button == SDL_BUTTON_LEFT) {
                const auto point = display.map_point(event.button.x, event.button.y, false);
                if (point) {
                    pointer_is_down = true;
                    browser.pointer_down(point->x, point->y);
                }
            }
            break;
        case SDL_MOUSEMOTION: {
            const auto point = display.map_point(event.motion.x, event.motion.y, pointer_is_down);
            if (point) browser.pointer_move(point->x, point->y);
            break;
        }
        case SDL_MOUSEBUTTONUP:
            if (event.button.button == SDL_BUTTON_LEFT && pointer_is_down) {
                const auto point = display.map_point(event.button.x, event.button.y, true);
                if (point) browser.pointer_up(point->x, point->y);
                else browser.pointer_up(-1, -1);
                pointer_is_down = false;
            }
            break;
        case SDL_MOUSEWHEEL: {
            int delta = event.wheel.y;
            if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                delta = delta == std::numeric_limits<int>::min()
                    ? std::numeric_limits<int>::max() : -delta;
            }
            if (delta != 0) browser.scroll(delta);
            break;
        }
        case SDL_KEYDOWN: {
            const std::optional<Key> key = translate_key(event.key.keysym.sym);
            if (key && !(event.key.repeat != 0 && *key == Key::toggle_stats)) {
                browser.key(*key);
            }
            break;
        }
        case SDL_TEXTINPUT:
            browser.text_input(event.text.text);
            break;
        default:
            break;
        }
    }
    return true;
}

void write_ppm(const Browser& browser, const std::filesystem::path& path) {
    const Size size = browser.screen_size();
    std::vector<char> row(static_cast<std::size_t>(size.width) * 3);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot open screenshot '" + path.string() + "'");
    output << "P6\n" << size.width << ' ' << size.height << "\n255\n";
    for (int y = 0; y < size.height; ++y) {
        const std::size_t row_offset = static_cast<std::size_t>(y) * size.width;
        for (int x = 0; x < size.width; ++x) {
            const Color color = from_rgb565(browser.pixels()[row_offset + x]);
            row[static_cast<std::size_t>(x) * 3] = static_cast<char>(color.red());
            row[static_cast<std::size_t>(x) * 3 + 1] = static_cast<char>(color.green());
            row[static_cast<std::size_t>(x) * 3 + 2] = static_cast<char>(color.blue());
        }
        output.write(row.data(), static_cast<std::streamsize>(row.size()));
    }
    output.close();
    if (!output) {
        throw std::runtime_error("failed while writing screenshot '" + path.string() + "'");
    }
}

Damage full_screen_damage(Size size) {
    Damage damage;
    damage.regions[0] = Rect{0, 0, size.width, size.height};
    damage.count = 1;
    return damage;
}

void print_report(const Options& options, const Browser& browser, uint64_t application_frames,
                  uint64_t uploaded_pixels, std::chrono::steady_clock::duration elapsed) {
    const double elapsed_ms = std::chrono::duration<double, std::milli>(elapsed).count();
    const Metrics& metrics = browser.metrics();
    const Size size = browser.screen_size();
    const uint64_t framebuffer_bytes = static_cast<uint64_t>(size.width) *
        static_cast<uint64_t>(size.height) * sizeof(uint16_t);

    std::cout << "Host timing (native desktop only, not an ESP32 benchmark): "
              << elapsed_ms << " ms for " << application_frames << " application frames";
    if (application_frames != 0) {
        std::cout << " (" << elapsed_ms / static_cast<double>(application_frames) << " ms/frame)";
    }
    std::cout << "\nRendered pixels: " << uploaded_pixels
              << " in damage regions; framebuffer " << size.width << 'x' << size.height
              << " RGB565 (" << framebuffer_bytes << " bytes)\n";
    std::cout << "Browser metrics: rendered frames=" << metrics.frames
              << ", scripts=" << metrics.scripts
              << ", layouts=" << metrics.layout_count
              << ", last layout=" << metrics.last_layout_us << " us"
              << ", last render=" << metrics.last_render_us << " us"
              << ", active HTTP requests=" << metrics.active_requests << '\n';

    if (options.spi_hz) {
        const uint64_t bits_per_frame = static_cast<uint64_t>(size.width) *
            static_cast<uint64_t>(size.height) * 16U;
        const long double frequency = static_cast<long double>(*options.spi_hz);
        const long double frames_per_second = frequency / static_cast<long double>(bits_per_frame);
        const long double milliseconds_per_frame =
            static_cast<long double>(bits_per_frame) * 1000.0L / frequency;
        std::cout << "Estimated SPI bus ceiling (ideal raw pixel transfer only; not measured, "
                     "not an ESP32 benchmark): "
                  << static_cast<double>(frames_per_second) << " full frames/s, "
                  << static_cast<double>(milliseconds_per_frame)
                  << " ms/full frame at " << *options.spi_hz << " Hz, excluding protocol overhead\n";
    }
    if (!browser.last_error().empty()) std::cerr << "Browser last error: " << browser.last_error() << '\n';
}

int run(const Options& options) {
    if (options.headless && SDL_setenv("SDL_VIDEODRIVER", "dummy", 1) != 0) {
        throw sdl_error("cannot select the SDL dummy video driver");
    }

    DesktopPlatform platform;
    SdlContext sdl;
    SdlDisplay display(options.size, options.scale);
    Browser browser(platform, options.size);
    const std::string package_path = options.package.string();
    browser.load(read_package_file(package_path.c_str()));

    std::optional<EventReplay> replay;
    if (options.has_events) replay.emplace(options.events);

    const auto host_started = std::chrono::steady_clock::now();
    const Damage initial_damage = full_screen_damage(options.size);
    const bool initially_changed = browser.tick();
    uint64_t uploaded_pixels = display.present(
        browser.pixels(), initially_changed ? &browser.damage() : &initial_damage);

    bool running = true;
    bool pointer_is_down = false;
    uint64_t application_frames = 0;
    const std::optional<uint64_t> frame_limit = options.frame_limit
        ? options.frame_limit
        : options.headless ? std::optional<uint64_t>(default_headless_frames) : std::nullopt;

    while (running && (!frame_limit || application_frames < *frame_limit)) {
        const auto frame_started = std::chrono::steady_clock::now();
        running = process_sdl_events(browser, display, pointer_is_down);
        if (!running) break;
        if (replay) replay->advance(browser, &write_ppm);
        const bool changed = browser.tick();
        if (changed || display.needs_present()) {
            uploaded_pixels += display.present(browser.pixels(), changed ? &browser.damage() : nullptr);
        }
        ++application_frames;
        if (!frame_limit || application_frames < *frame_limit) {
            std::this_thread::sleep_until(frame_started + frame_interval);
        }
    }

    if (running && frame_limit && application_frames >= *frame_limit && replay && !replay->finished()) {
        throw std::runtime_error("event replay did not finish within " + std::to_string(*frame_limit) +
                                 " application frames; increase --frames for the replay or its waits");
    }
    if (options.has_screenshot) write_ppm(browser, options.screenshot);
    print_report(options, browser, application_frames, uploaded_pixels,
                 std::chrono::steady_clock::now() - host_started);
    return 0;
}

} // namespace
} // namespace mb

int main(int argc, char* argv[]) {
    using namespace mb;
    try {
        const Options options = parse_options(argc, argv);
        if (options.help) {
            print_usage(std::cout);
            return 0;
        }
        return run(options);
    } catch (const UsageError& error) {
        std::cerr << "Error: " << error.what() << '\n';
        print_usage(std::cerr);
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
