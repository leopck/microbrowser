#include "package_identity.h"

#include "microbrowser/browser.h"
#include "platform/zephyr_peripherals.h"
#if defined(CONFIG_MB_BLE_CENTRAL)
#include "platform/zephyr_ble_central.h"
#endif
#include "platform/zephyr_event_link.h"
#include "platform/zephyr_platform.h"

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#if defined(CONFIG_MB_PACKAGE_FETCH_ENABLED)
#include <psa/crypto.h>
#endif


#if defined(MB_BSIM_COMPLETION_TEST)
#include "nsi_main.h"
#endif
#if defined(MB_EMBED_TLS_CA_CERTIFICATE)
#include <zephyr/net/tls_credentials.h>
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

LOG_MODULE_REGISTER(mb_nrf_app, LOG_LEVEL_INF);

namespace {

constexpr mb::Size kHeadlessScreen{240, 180};
constexpr std::string_view kButtonName = "user_button";
constexpr std::array<mb::DeviceOperation, 7> kButtonOperations{
    mb::DeviceOperation::open, mb::DeviceOperation::configure,
    mb::DeviceOperation::read, mb::DeviceOperation::subscribe,
    mb::DeviceOperation::unsubscribe, mb::DeviceOperation::cancel,
    mb::DeviceOperation::close,
};
constexpr std::string_view kStatusLedName = "status_led";
constexpr std::array<mb::DeviceOperation, 6> kStatusLedOperations{
    mb::DeviceOperation::open, mb::DeviceOperation::configure,
    mb::DeviceOperation::read, mb::DeviceOperation::write,
    mb::DeviceOperation::cancel, mb::DeviceOperation::close,
};
constexpr uint32_t kPackageDownloadId = 1;
constexpr uint32_t kApplicationPeriodMs = 10;
constexpr uint32_t kMaximumPackageBytes = 16U * 1024U * 1024U;
constexpr int64_t kProofIntervalMs = 2000;
static_assert(CONFIG_MB_PACKAGE_MAX_BYTES > 0 && CONFIG_MB_PACKAGE_MAX_BYTES <= kMaximumPackageBytes,
              "the configured package limit must be within the v3 format limit");

#if !DT_NODE_HAS_STATUS(DT_ALIAS(sw0), okay)
#error "The board profile must provide an enabled sw0 button for user_button"
#endif

#if defined(CONFIG_MB_EVENT_LINK_ENABLED)
#if !DT_NODE_HAS_STATUS(DT_ALIAS(sw1), okay)
#error "MB_EVENT_LINK_ENABLED requires an enabled sw1 source button"
#endif
#if defined(CONFIG_MB_STATUS_LED_ENABLED)
#if !DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
#error "MB_STATUS_LED_ENABLED requires an enabled led0 alias"
#endif
#endif
nrfx_gpiote_t event_link_gpiote = NRFX_GPIOTE_INSTANCE(NRF_GPIOTE);
nrfx_timer_t event_link_timer = NRFX_TIMER_INSTANCE(NRF_TIMER2);
#endif

const uint8_t kEmbeddedPackage[] = {
#include "microbrowser_package.inc"
};

#if defined(MB_EMBED_TLS_CA_CERTIFICATE)
const uint8_t kEmbeddedTlsCa[] = {
#include "microbrowser_tls_ca.der.inc"
};
#endif

std::string sanitized_line(std::string_view message) {
    constexpr std::size_t maximum = 384;
    std::string output;
    output.reserve(std::min(message.size(), maximum) + 1);
    const std::size_t count = std::min(message.size(), maximum);
    for (std::size_t index = 0; index < count; ++index) {
        const unsigned char byte = static_cast<unsigned char>(message[index]);
        output.push_back(byte >= 0x20U && byte != 0x7fU ? char(byte) : '?');
    }
    if (message.size() > maximum) output.back() = '~';
    return output;
}

uint16_t read_little_u16(std::span<const uint8_t> bytes, std::size_t offset) {
    return uint16_t(bytes[offset]) | (uint16_t(bytes[offset + 1]) << 8U);
}

uint32_t read_little_u32(std::span<const uint8_t> bytes, std::size_t offset) {
    uint32_t result = 0;
    for (unsigned index = 0; index < 4; ++index) result |= uint32_t(bytes[offset + index]) << (8U * index);
    return result;
}

void validate_v3_header(std::span<const uint8_t> bytes, std::size_t maximum_bytes) {
    if (bytes.size() < 20 || bytes.size() > maximum_bytes || maximum_bytes > kMaximumPackageBytes) {
        throw mb::PackageError("package is outside the board's configured byte limits");
    }
    if (bytes[0] != 'M' || bytes[1] != 'B' || bytes[2] != 'P' || bytes[3] != 'K' ||
        read_little_u16(bytes, 4) != 3 || read_little_u16(bytes, 6) != 0) {
        throw mb::PackageError("board applications require a binary MBPK version 3.0 package");
    }
    if (read_little_u32(bytes, 8) != bytes.size()) {
        throw mb::PackageError("package header size does not match the embedded/downloaded byte count");
    }
}

#if defined(MB_EMBED_TLS_CA_CERTIFICATE)
void install_owner_tls_ca() {
    const int error = tls_credential_add(static_cast<sec_tag_t>(CONFIG_MB_TLS_CA_SEC_TAG),
                                         TLS_CREDENTIAL_CA_CERTIFICATE,
                                         kEmbeddedTlsCa, sizeof(kEmbeddedTlsCa));
    if (error != 0) throw std::runtime_error("could not provision the owner's CA security tag: " + std::to_string(error));
}
#endif

#if defined(CONFIG_MB_PACKAGE_FETCH_ENABLED)
std::array<char, 65> sha256_hex(std::span<const uint8_t> bytes) {
    std::array<uint8_t, PSA_HASH_LENGTH(PSA_ALG_SHA_256)> digest{};
    std::size_t digest_bytes = 0;
    const psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256, bytes.data(), bytes.size(),
                                                 digest.data(), digest.size(), &digest_bytes);
    if (status != PSA_SUCCESS || digest_bytes != digest.size()) {
        throw std::runtime_error("PSA SHA-256 failed for a downloaded package");
    }
    static constexpr char digits[] = "0123456789abcdef";
    std::array<char, 65> hexadecimal{};
    for (std::size_t index = 0; index < digest.size(); ++index) {
        hexadecimal[index * 2] = digits[digest[index] >> 4U];
        hexadecimal[index * 2 + 1] = digits[digest[index] & 15U];
    }
    return hexadecimal;
}
#endif

mb::ZephyrHttpConfig http_configuration() {
    return mb::ZephyrHttpConfig{
        .enabled = IS_ENABLED(CONFIG_MB_HTTP_ENABLED),
        .allow_plain_http = IS_ENABLED(CONFIG_MB_ALLOW_PLAIN_HTTP),
        .tls_ca_security_tag = CONFIG_MB_TLS_CA_SEC_TAG,
        .wifi_connect_timeout_ms = CONFIG_MB_WIFI_CONNECT_TIMEOUT_MS,
        .socket_connect_timeout_ms = CONFIG_MB_SOCKET_CONNECT_TIMEOUT_MS,
        .request_timeout_ms = CONFIG_MB_HTTP_REQUEST_TIMEOUT_MS,
        .shutdown_timeout_ms = CONFIG_MB_HTTP_SHUTDOWN_TIMEOUT_MS,
        .receive_buffer_bytes = CONFIG_MB_HTTP_RECEIVE_BUFFER_BYTES,
        .script_response_limit_bytes = CONFIG_MB_SCRIPT_RESPONSE_LIMIT_BYTES,
        .content_response_limit_bytes = CONFIG_MB_CONTENT_RESPONSE_LIMIT_BYTES,
    };
}

class NrfApplication final {
public:
    NrfApplication()
        : m_platform(http_configuration()),
#if defined(CONFIG_MB_BLE_CENTRAL)
          m_ble_central(mb::zephyr::register_zephyr_ble_central(
              m_registry, mb::zephyr::ZephyrBleCentralConfig{
                  "ble_central", CONFIG_MB_BLE_SCAN_TIMEOUT_MS,
                  mb::EndpointAccess::application})),
#endif
          m_button(mb::zephyr::register_zephyr_gpio(
              m_registry, k_sys_work_q,
              mb::zephyr::ZephyrGpioConfig{
                  kButtonName, GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios),
                  mb::zephyr::GpioDirectionPermission::input_only,
                  std::span<const mb::DeviceOperation>(kButtonOperations),
                  mb::EndpointAccess::application})),
#if defined(CONFIG_MB_STATUS_LED_ENABLED)
          m_status_led(mb::zephyr::register_zephyr_gpio(
              m_registry, k_sys_work_q,
              mb::zephyr::ZephyrGpioConfig{
                  kStatusLedName, GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),
                  mb::zephyr::GpioDirectionPermission::output_only,
                  std::span<const mb::DeviceOperation>(kStatusLedOperations),
                  mb::EndpointAccess::application})),
#endif
          m_browser(std::make_unique<mb::Browser>(
            m_platform, kHeadlessScreen, CONFIG_MB_GATEWAY_URL, &m_registry)) {
#if defined(CONFIG_MB_EVENT_LINK_ENABLED)
        m_event_link = mb::zephyr::register_zephyr_event_link(
            m_registry, {"button_event", GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios),
                         mb::GpioEdge::both, &event_link_gpiote, &event_link_timer});
#endif
    }

    NrfApplication(const NrfApplication&) = delete;
    NrfApplication& operator=(const NrfApplication&) = delete;

    [[noreturn]] void run() {
        std::vector<uint8_t> embedded(std::begin(kEmbeddedPackage), std::end(kEmbeddedPackage));
        load_package(std::move(embedded), "embedded", MB_EMBEDDED_PACKAGE_SHA256);
#if defined(MB_EMBED_TLS_CA_CERTIFICATE)
        install_owner_tls_ca();
#endif
#if defined(CONFIG_MB_HTTP_ENABLED)
        m_platform.start_network();
#endif
#if defined(CONFIG_MB_PACKAGE_FETCH_ENABLED)
        if (std::string_view(CONFIG_MB_PACKAGE_URL).empty()) {
            throw std::invalid_argument("package fetching is enabled without an owner-provided URL");
        }
        if (psa_crypto_init() != PSA_SUCCESS) throw std::runtime_error("PSA crypto initialization failed");
        m_platform.start_package_download(kPackageDownloadId, CONFIG_MB_PACKAGE_URL,
                                          CONFIG_MB_PACKAGE_MAX_BYTES);
        m_package_download_active = true;
#endif
        m_browser->tick();
        emit_graphics_proof();
        m_last_proof_ms = k_uptime_get();
        int64_t next_wake = m_last_proof_ms;
        for (;;) {
            next_wake += kApplicationPeriodMs;
            const bool rendered = m_browser->tick();
            m_graphics_proof_pending = m_graphics_proof_pending || rendered;
#if defined(MB_BSIM_COMPLETION_TEST)
#if defined(CONFIG_MB_BLE_CENTRAL)
            step_bsim_gatt_completion();
#else
            step_bsim_gpio_completion();
#endif
#endif
#if defined(CONFIG_MB_PACKAGE_FETCH_ENABLED)
            poll_package_download();
#endif
            const int64_t now = k_uptime_get();
            if (m_graphics_proof_pending && now - m_last_proof_ms >= kProofIntervalMs) {
                emit_graphics_proof();
                m_last_proof_ms = now;
            }
            if (next_wake > now) {
                k_msleep(int32_t(std::min<int64_t>(next_wake - now, std::numeric_limits<int32_t>::max())));
            } else {
                next_wake = now;
            }
        }
    }

private:

#if defined(MB_BSIM_COMPLETION_TEST)
    [[noreturn]] void complete_bsim_run(std::string_view summary) {
        if (!m_browser->last_error().empty()) {
            throw std::runtime_error(std::string("simulated GPIO close failed: ") +
                                     std::string(m_browser->last_error()));
        }
#if defined(MB_BSIM_COMPLETION_TEST) && defined(CONFIG_MB_EVENT_LINK_ENABLED)
        if (m_event_link->shutdown() != mb::IoStatus::completed) {
            throw std::runtime_error("simulated event-link driver did not shut down");
        }
#endif
        emit_graphics_proof();
#if defined(CONFIG_MB_STATUS_LED_ENABLED)
        if (m_status_led->shutdown() != mb::IoStatus::completed) {
            throw std::runtime_error("simulated status LED driver did not shut down");
        }
#endif
#if defined(CONFIG_MB_BLE_CENTRAL)
        if (m_ble_central->shutdown() != mb::IoStatus::completed) {
            throw std::runtime_error("simulated GATT driver did not shut down");
        }
#endif
        if (m_button->shutdown() != mb::IoStatus::completed) {
            throw std::runtime_error("simulated GPIO driver did not shut down");
        }
        m_platform.stop_network();
        m_platform.log(summary);
        nsi_exit(0);
    }

    void step_bsim_gatt_completion() {
        constexpr int64_t completion_deadline_ms = 60'000;
        const int64_t now = k_uptime_get();
        if (m_platform.log_count("GATT conformance failed") != 0 ||
            !m_browser->last_error().empty()) {
            throw std::runtime_error(std::string("simulated GATT failed: ") +
                                     std::string(m_browser->last_error()));
        }
        if (m_platform.log_count("GATT conformance complete") == 0) {
            if (now > completion_deadline_ms) {
                throw std::runtime_error("native GATT conformance exceeded the Bsim deadline");
            }
            return;
        }
        if (m_platform.log_count("GATT value verified after write") != 1 ||
            m_platform.log_count("GATT notification verified") != 1 ||
            m_platform.log_count("GATT readonly rejected") != 1 ||
            m_platform.log_count("GATT disconnected callback verified") != 1) {
            throw std::runtime_error("GATT completion lacked expected peer observations");
        }
        complete_bsim_run(
            "MICROBROWSER_BSIM_PASS package=embedded callbacks=gatt close=completed");
    }

    void step_bsim_gpio_completion() {
        constexpr int64_t close_at_ms = 26'000;
        constexpr int64_t completion_timeout_ms = 2'000;
        const int64_t now = k_uptime_get();
        if (!m_finish_requested) {
            if (now < close_at_ms) return;
            if (m_platform.log_count("GPIO subscription ready") != 1 ||
                m_platform.log_count("GPIO notification") < 2) {
                throw std::runtime_error("simulated GPIO events did not reach the authored callbacks");
            }
            m_browser->dispatch("finish");
            m_finish_requested = true;
            m_finish_requested_at_ms = now;
            return;
        }
        complete_bsim_run(
            "MICROBROWSER_BSIM_PASS package=embedded callbacks=gpio close=completed");
        if (now - m_finish_requested_at_ms > completion_timeout_ms) {
            throw std::runtime_error("application GPIO unsubscribe/close exceeded the Bsim deadline");
        }
    }
#endif
    void load_package(std::vector<uint8_t> bytes, std::string_view source, std::string_view digest) {
        validate_v3_header(bytes, CONFIG_MB_PACKAGE_MAX_BYTES);
        const std::size_t payload_bytes = bytes.size();
        m_browser->load(std::move(bytes));
        std::string summary = "MICROBROWSER_PACKAGE source=";
        summary += source;
        summary += " payload_bytes=" + std::to_string(payload_bytes);
        summary += " manifest_digest_sha256=";
        summary += digest;
        summary += " format=v3.0";
        m_platform.log(summary);
    }

#if defined(CONFIG_MB_PACKAGE_FETCH_ENABLED)
    void poll_package_download() {
        if (!m_package_download_active) return;
        mb::HttpResult result;
        if (!m_platform.poll_package_download(result)) return;
        m_package_download_active = false;
        if (result.id != kPackageDownloadId) throw std::runtime_error("package completion identifier mismatch");
        if (!result.succeeded()) {
            std::string message = "MICROBROWSER_PACKAGE_FETCH status=" + std::to_string(result.status);
            message += " retry=disabled result=retained_embedded_package";
            if (!result.error.empty()) message += " error=" + result.error;
            m_platform.log(message);
            return;
        }
        if (result.body.size() > CONFIG_MB_PACKAGE_MAX_BYTES) {
            throw std::runtime_error("completed package exceeds the configured byte limit");
        }
        const auto digest = sha256_hex(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(result.body.data()), result.body.size()));
        std::vector<uint8_t> package(result.body.begin(), result.body.end());
        load_package(std::move(package), "download", std::string_view(digest.data(), digest.size() - 1));
        m_graphics_proof_pending = true;
    }
#endif

    uint64_t frame_digest() const {
        uint64_t hash = 14695981039346656037ULL;
        const std::size_t pixels = std::size_t(kHeadlessScreen.width) * std::size_t(kHeadlessScreen.height);
        for (std::size_t index = 0; index < pixels; ++index) {
            const uint16_t pixel = m_browser->pixels()[index];
            hash ^= uint8_t(pixel);
            hash *= 1099511628211ULL;
            hash ^= uint8_t(pixel >> 8U);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    uint64_t damage_pixels() const {
        uint64_t pixels = 0;
        const mb::Damage& damage = m_browser->damage();
        for (std::size_t index = 0; index < damage.count; ++index) {
            const mb::Rect& region = damage.regions[index];
            if (!region.empty()) pixels += uint64_t(region.width) * uint64_t(region.height);
        }
        return pixels;
    }

    void emit_graphics_proof() {
        std::array<char, 512> line{};
        const mb::Metrics& metrics = m_browser->metrics();
        const std::string_view page = m_browser->page_name();
        const int length = std::snprintf(
            line.data(), line.size(),
            "MICROBROWSER_HEADLESS_FRAME frame=%llu page=%.*s framebuffer=%dx%d display=none "
            "rgb565_fnv1a64=%016llx damage_pixels=%llu rendered_pixels_total=%llu",
            static_cast<unsigned long long>(metrics.frames), int(page.size()), page.data(),
            kHeadlessScreen.width, kHeadlessScreen.height,
            static_cast<unsigned long long>(frame_digest()),
            static_cast<unsigned long long>(damage_pixels()),
            static_cast<unsigned long long>(metrics.flushed_pixels));
        if (length < 0 || std::size_t(length) >= line.size()) throw std::runtime_error("headless proof exceeds its log bound");
        m_platform.log(std::string_view(line.data(), std::size_t(length)));
        m_graphics_proof_pending = false;
    }

    mb::ZephyrPlatform m_platform;
    mb::PeripheralRegistry m_registry;
#if defined(CONFIG_MB_BLE_CENTRAL)
    std::shared_ptr<mb::zephyr::ZephyrBleCentralBackend> m_ble_central;
#endif
    std::shared_ptr<mb::zephyr::ZephyrGpioBackend> m_button;
#if defined(CONFIG_MB_STATUS_LED_ENABLED)
    std::shared_ptr<mb::zephyr::ZephyrGpioBackend> m_status_led;
#endif
    std::unique_ptr<mb::Browser> m_browser;
    int64_t m_last_proof_ms = 0;
#if defined(CONFIG_MB_EVENT_LINK_ENABLED)
    std::shared_ptr<mb::zephyr::ZephyrEventLinkBackend> m_event_link;
#endif
    bool m_graphics_proof_pending = false;
    bool m_package_download_active = false;
#if defined(MB_BSIM_COMPLETION_TEST)
    bool m_finish_requested = false;
    int64_t m_finish_requested_at_ms = 0;
#endif
};

} // namespace

int main() {
    try {
        NrfApplication application;
        application.run();
    } catch (const std::exception& error) {
        const std::string reason = sanitized_line(error.what());
        printk("MICROBROWSER_FATAL %s\n", reason.c_str());
        std::abort();
    }
}
