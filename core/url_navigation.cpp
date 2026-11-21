#include "url_state.h"

#include "microbrowser/canvas.h"

#include <algorithm>

#include <charconv>
#include <cstring>
#include <system_error>

namespace mb {
namespace {

constexpr std::size_t max_website_url_bytes = 2047;
constexpr std::size_t max_gateway_bytes = 8192;
constexpr std::size_t max_history_entries = 16;
constexpr std::size_t max_image_bytes = 2U * 1024U * 1024U;
constexpr std::size_t max_pending_images = 4;
constexpr uint16_t maximum_segment_height = 640;
constexpr uint32_t maximum_scaled_image_height = 32768;
constexpr unsigned fnv_offset = 2166136261U;
constexpr unsigned fnv_prime = 16777619U;
void append_bounded_url(std::vector<std::string>& urls, const std::string& url) {
    if (urls.size() >= max_history_entries) urls.erase(urls.begin());
    urls.push_back(url);
}


[[nodiscard]] bool ascii_equal(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        char a = left[index];
        char b = right[index];
        if (a >= 'A' && a <= 'Z') a = char(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = char(b + ('a' - 'A'));
        if (a != b) return false;
    }
    return true;
}
[[nodiscard]] bool absolute_http_url(std::string_view url) {
    return (url.size() >= 7 && ascii_equal(url.substr(0, 7), "http://")) ||
        (url.size() >= 8 && ascii_equal(url.substr(0, 8), "https://"));
}


void validate_url(std::string_view url, std::size_t limit) {
    if (url.empty() || url.size() > limit) throw ScriptError("URL exceeds its supported length");
    const bool http = url.size() >= 7 && ascii_equal(url.substr(0, 7), "http://");
    const bool https = url.size() >= 8 && ascii_equal(url.substr(0, 8), "https://");
    if (!http && !https) throw ScriptError("URLs must use HTTP or HTTPS");
    const std::size_t authority = url.find("://") + 3;
    const std::size_t end = url.find_first_of("/?#", authority);
    const std::string_view host = url.substr(authority, end - authority);
    if (host.empty() || host.front() == '@' || host.back() == '@') throw ScriptError("URL has no valid host");
    if (url.find('@', authority) < (end == std::string_view::npos ? url.size() : end)) {
        throw ScriptError("URLs containing embedded credentials are not supported");
    }
    for (const unsigned char character : url) {
        if (character <= 0x20U || character == 0x7fU) throw ScriptError("URL contains whitespace or control characters");
    }
}

std::string trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return std::string(value);
}

std::string normalize_url(std::string_view input) {
    std::string url = trim(input);
    if (url == "mb://home") return url;
    if (url.find("://") == std::string::npos) url = "https://" + url;
    if (url.size() > max_website_url_bytes) {
        throw ScriptError("website URLs are limited to 2047 bytes");
    }
    if (ascii_equal(url.substr(0, 7), "http://")) url.replace(0, 7, "http://");
    if (ascii_equal(url.substr(0, 8), "https://")) url.replace(0, 8, "https://");
    validate_url(url, max_website_url_bytes);
    return url;
}

std::string origin_of(std::string_view url) {
    const std::size_t start = url.find("://") + 3;
    const std::size_t end = url.find_first_of("/?#", start);
    return std::string(url.substr(0, end));
}

std::string join_url(std::string_view base, std::string_view reference,
                     std::size_t limit = max_website_url_bytes) {
    if (reference.empty()) throw ScriptError("document contains an empty URL");
    if (absolute_http_url(reference)) return normalize_url(reference);
    if (reference.starts_with("//")) return normalize_url(std::string(base.substr(0, base.find(':') + 1)) + std::string(reference));
    std::string url;
    if (reference.front() == '/') url = origin_of(base);
    else {
        const std::size_t query = base.find_first_of("?#");
        const std::size_t end = query == std::string_view::npos ? base.size() : query;
        const std::size_t slash = base.rfind('/', end);
        if (slash == std::string_view::npos || slash < base.find("://") + 3) throw ScriptError("URL has no base path");
        url = std::string(base.substr(0, slash + 1));
    }
    url += reference;
    validate_url(url, limit);
    return url;
}

std::string encode_query(std::string_view value) {
    constexpr std::string_view digits = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size() * 3);
    for (const unsigned char byte : value) {
        const bool literal = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' || byte == '~';
        if (literal) result.push_back(static_cast<char>(byte));
        else {
            result.push_back('%');
            result.push_back(digits[byte >> 4U]);
            result.push_back(digits[byte & 15U]);
        }
    }
    return result;
}

std::string encode_json(std::string_view value) {
    std::string result;
    for (const unsigned char character : value) {
        if (character < 0x20U || character == 0x7fU) throw ScriptError("URL contains a control character");
        if (character == '"' || character == '\\') result.push_back('\\');
        result.push_back(static_cast<char>(character));
    }
    return result;
}

std::string reader_request_url(std::string_view gateway, std::string_view url) {
    std::string request = std::string(gateway) + "/v1/reader?url=" + encode_query(url);
    validate_url(request, max_gateway_bytes);
    return request;
}
std::string reader_image_request_url(std::string_view gateway, std::string_view source) {
    std::string absolute;
    if (absolute_http_url(source)) absolute = normalize_url(source);
    else if (source.starts_with("//")) absolute = join_url(gateway, source);
    else return join_url(gateway, source, max_gateway_bytes);
    std::string request = std::string(gateway) + "/v1/asset?url=" + encode_query(absolute);
    validate_url(request, max_gateway_bytes);
    return request;
}


[[nodiscard]] bool path_ends_with(std::string_view url, std::string_view extension) {
    const std::size_t query = url.find_first_of("?#");
    const std::string_view path = url.substr(0, query);
    return path.size() >= extension.size() &&
        ascii_equal(path.substr(path.size() - extension.size()), extension);
}

[[nodiscard]] bool is_package_url(std::string_view url) {
    return path_ends_with(url, ".mbp");
}

[[nodiscard]] bool is_direct_document_url(std::string_view url) {
    return path_ends_with(url, ".mbr.json");
}

[[nodiscard]] bool is_package(const std::string& body) {
    return body.size() >= 4 && std::memcmp(body.data(), "MBPK", 4) == 0;
}

[[nodiscard]] bool supported_json(std::string_view type) {
    return type == "application/json" || type == "application/vnd.microbrowser.reader+json";
}

uint32_t decimal_header(const std::string& text, std::string_view name, uint32_t maximum, bool required = true) {
    if (text.empty() && !required) return 0;
    uint32_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value > maximum) {
        throw PackageError("invalid " + std::string(name) + " image metadata");
    }
    return value;
}

void count_nodes(const Node& node, uint32_t& count) {
    if (++count > 2048) throw PackageError("Reader exceeds 2048 elements");
    for (const auto& child : node.children) count_nodes(*child, count);
}


Node* find_image(Node& node, std::string_view asset) {
    if (node.kind == NodeKind::image && node.source == asset) return &node;
    for (auto& child : node.children) {
        if (Node* found = find_image(*child, asset)) return found;
    }
    return nullptr;
}

Node* find_image_parent(Node& node, std::string_view asset) {
    for (auto& child : node.children) {
        if (child->kind == NodeKind::image && child->source == asset) return &node;
        if (Node* found = find_image_parent(*child, asset)) return found;
    }
    return nullptr;
}

std::string purpose_name(TransferKind kind) {
    switch (kind) {
    case TransferKind::native_package: return "application package";
    case TransferKind::reader: return "Reader document";
    case TransferKind::direct_document: return "static Reader document";
    case TransferKind::interactive: return "interactive document";
    }
    throw PackageError("unknown document transfer kind");
}

} // namespace

std::string validated_gateway_origin(const std::string& value) {
    validate_url(value, max_gateway_bytes);
    const std::string origin = origin_of(value);
    if (value != origin && value != origin + "/") throw ScriptError("gateway must be an HTTP origin without a path or query");
    return origin;
}

uint32_t Browser::UrlState::active_requests() const {
    return uint32_t(pending_images.size() + (pending_transfer.has_value() ? 1U : 0U));
}

Browser::~Browser() = default;

std::string_view Browser::location() const { return m_url->location; }
std::string_view Browser::content_mode() const {
    if (!m_url->active()) return "application";
    return m_url->mode == ReaderMode::interactive ? "interactive" : "reader";
}

void Browser::clear_url_view(bool forget_history) {
    cancel_pointer_gesture();
    close_devices();
    m_focus = nullptr;
    m_show_keyboard = false;
    m_url->document.reset();
    m_url->generated_page.reset();
    m_url->pending_transfer.reset();
    m_url->pending_images.clear();
    m_url->images.clear();
    m_url->session.clear();
    m_url->cached_image_bytes = 0;
    m_url->mode = ReaderMode::reader;
    if (forget_history) m_url->history.clear();
}


void Browser::install_reader_page(ReaderDocument document, const std::string& address) {
    clear_url_view(false);
    m_url->mode = document.mode;
    m_url->session = document.session.value_or("");
    m_url->document = std::move(document);
    auto generated = std::make_unique<RemotePage>(make_reader_page(*m_url->document, address, m_size));
    m_page = nullptr;
    m_history_size = 0;
    m_timers = {};
    m_requests = {};
    m_error.clear();
    m_error_until = 0;
    m_package = {};
    m_state.clear();
    for (auto& field : generated->states) m_state.push_back(field.initial);
    m_package.pages.push_back(std::move(generated->page));
    m_url->generated_page = std::move(generated);
    m_metrics = {};
    m_metrics.state_count = uint32_t(m_state.size());
    for (const Page& page : m_package.pages) count_nodes(page.root, m_metrics.node_count);
    for (std::size_t index = 0; index < m_url->document->blocks.size(); ++index) {
        const ReaderBlock& block = m_url->document->blocks[index];
        if (block.kind != ReaderBlockKind::image) continue;
        const std::string asset = "remote-block:" + std::to_string(index);
        Node* node = find_image(m_package.pages[0].root, asset);
        if (node == nullptr) throw PackageError("Reader page is missing an image placeholder");
        m_package.assets.push_back(Asset{asset, 1, 1, {to_rgb565(palette::surface)}});
        m_url->images.push_back({asset, reader_image_request_url(m_url->gateway_url, block.url), node, index,
                                  0, 0, 0, 0, false, false, false, false});
    }
    open_page_index(0, false);
    queue_visible_images();
}

void Browser::fail_url_load(const std::string& message) {
    m_platform.log(message);
    ReaderDocument document;
    document.kind = ReaderKind::unsupported;
    document.title = "Unable to open URL";
    document.source_url = m_url->location.empty() ? std::string("mb://home") : m_url->location;
    document.unsupported_reason = message;
    document.mode = m_url->mode;
    install_reader_page(std::move(document), m_url->location);
    m_error = message;
    m_error_until = m_platform.now_ms() + 5'000;
}

void Browser::open_reader_url(const std::string& raw_url, bool interactive, bool remember,
                              NativePackageIntent package_intent) {
    try {
        const std::string url = normalize_url(raw_url);
        if (remember && !m_url->location.empty()) {
            append_bounded_url(m_url->history, m_url->location);
        }
        m_url->location = url;
        if (package_intent == NativePackageIntent::confirmed &&
            (!m_url->document || m_url->document->kind != ReaderKind::native_package_confirmation ||
             m_url->document->source_url != url)) {
            throw ScriptError("native application confirmation is not current");
        }
        if (is_package_url(url) && !interactive && package_intent == NativePackageIntent::review) {
            if (std::find(m_url->approved_packages.begin(), m_url->approved_packages.end(), url) == m_url->approved_packages.end()) {
                ReaderDocument confirmation;
                confirmation.kind = ReaderKind::native_package_confirmation;
                confirmation.title = "Native application download";
                confirmation.source_url = url;
                confirmation.unsupported_reason = "A Microbrowser package contains bytecode. Open it only if you trust its author. Website HTML never takes this path.";
                ReaderBlock button;
                button.kind = ReaderBlockKind::link;
                button.text = "Run native Microbrowser application";
                button.url = url;
                confirmation.blocks.push_back(std::move(button));
                install_reader_page(std::move(confirmation), url);
                return;
            }
        }
        const auto kind = interactive ? TransferKind::interactive
            : is_package_url(url) ? TransferKind::native_package
            : is_direct_document_url(url) ? TransferKind::direct_document : TransferKind::reader;
        ReaderDocument loading;
        loading.kind = ReaderKind::unsupported;
        loading.title = "Loading website";
        loading.source_url = url;
        loading.mode = interactive ? ReaderMode::interactive : ReaderMode::reader;
        loading.unsupported_reason = interactive
            ? "Interactive mode is loading in your configured gateway's browser."
            : "Loading through the Reader gateway; website scripts are not executed on this device.";
        const std::string session = m_url->session;
        install_reader_page(std::move(loading), url);
        if (m_next_id > 16'777'215) throw ScriptError("content request identifier space is exhausted");
        const uint32_t id = uint32_t(m_next_id++);
        ContentRequest request;
        if (interactive) {
            request.url = m_url->gateway_url + "/v1/interactive";
            request.method = ContentRequestMethod::post;
            request.body = "{\"url\":\"" + encode_json(url) + "\",\"consent\":true";
            if (!session.empty()) request.body += ",\"session\":\"" + encode_json(session) + "\"";
            request.body += "}";
        } else if (kind == TransferKind::native_package || kind == TransferKind::direct_document) {
            request.url = url;
        } else request.url = reader_request_url(m_url->gateway_url, url);
        m_url->pending_transfer = {id, url, kind};
        m_platform.start_content(id, std::move(request));
        m_metrics.active_requests = uint32_t(m_url->active_requests());
    } catch (const std::exception& error) {
        fail_url_load(error.what());
    }
}

void Browser::open_url(std::string url, bool remember) {
    if (trim(url) == "mb://home") {
        if (!m_url->home_package.empty()) {
            if (remember && !m_url->location.empty()) append_bounded_url(m_url->history, m_url->location);
            const auto bytes = m_url->home_package;
            auto history = m_url->history;
            load(bytes);
            m_url->history = std::move(history);

        } else {
            ReaderDocument welcome;
            welcome.title = "Microbrowser";
            welcome.source_url = "mb://home";
            welcome.blocks.push_back({ReaderBlockKind::paragraph,
                "Enter a website URL above. Public articles and manga open in Reader; interactive sites require your consent.", {}, {}, {}});
            install_reader_page(std::move(welcome), {});
        }
        return;
    }
    open_reader_url(url, false, remember);
}

void Browser::open_interactive_url(std::string url) {
    open_reader_url(url, true, true);
}

void Browser::confirm_native_package() {
    if (!m_url->document || m_url->document->kind != ReaderKind::native_package_confirmation) {
        throw ScriptError("native application confirmation is not current");
    }
    open_reader_url(m_url->document->source_url, false, false, NativePackageIntent::confirmed);
}

void Browser::navigate_back() {
    if (!m_url->history.empty()) {
        std::string url = std::move(m_url->history.back());
        m_url->history.pop_back();
        open_url(std::move(url), false);
        return;
    }
    if (m_history_size != 0 && !m_url->active()) {
        open_page_index(m_history[--m_history_size], false);
        return;
    }
    if (m_url->active()) {
        // Direct URLs can have no prior entry; always offer a usable way back to Home.
        open_url("mb://home");
        return;
    }
    throw ScriptError("navigation history is empty");
}

void Browser::perform_remote_action(Node& node) {
    if (!m_url->active()) return;
    const auto found = std::find_if(m_url->generated_page->links.begin(), m_url->generated_page->links.end(),
        [&](const RemoteLink& link) { return link.node == &node; });
    if (found == m_url->generated_page->links.end()) return;
    const RemoteLink action = *found;
    switch (action.action) {
    case RemoteAction::submit_address: {
        if (node.binding >= m_state.size()) throw ScriptError("address field has no state binding");
        open_url(m_state[node.binding].string);
        return;
    }
    case RemoteAction::go_home: open_url("mb://home"); return;
    case RemoteAction::go_back: navigate_back(); return;
    case RemoteAction::reader_url: {
        const std::string url = join_url(m_url->document->source_url, action.url);
        if (m_url->document->mode == ReaderMode::interactive) open_reader_url(url, true, true);
        else open_url(url);
        return;
    }
    case RemoteAction::run_native_package: {
        if (!m_url->document || m_url->document->kind != ReaderKind::native_package_confirmation ||
            m_url->document->source_url != action.url) {
            throw ScriptError("native application confirmation is not current");
        }
        open_reader_url(action.url, false, false, NativePackageIntent::confirmed);
        return;
    }
    case RemoteAction::interactive_url: {
        const std::string url = join_url(m_url->document->source_url, action.url);
        open_reader_url(url, true, true);
        return;
    }
    case RemoteAction::none: return;
    default: throw ScriptError("unknown remote navigation action");
    }
}

void Browser::receive_transfer(HttpResult&& result) {
    const auto pending = std::move(*m_url->pending_transfer);
    m_url->pending_transfer.reset();
    if (pending.id != result.id) return;
    if (!result.succeeded()) {
        fail_url_load("Could not load " + purpose_name(pending.kind) + ": " +
            (result.error.empty() ? "HTTP status " + std::to_string(result.status) : result.error));
        return;
    }
    if (pending.kind == TransferKind::native_package) {
        if (!is_package(result.body)) throw PackageError("downloaded application is not a Microbrowser package");
        const std::vector<uint8_t> bytes(result.body.begin(), result.body.end());
        uint32_t hash = fnv_offset;
        for (const unsigned char character : origin_of(pending.original_url)) hash = (hash ^ character) * fnv_prime;
        constexpr std::string_view digits = "0123456789abcdef";
        m_storage_prefix = "app-";
        for (unsigned shift = 28; shift < 32; shift -= 4) m_storage_prefix.push_back(digits[(hash >> shift) & 15U]);
        m_storage_prefix.push_back(':');
        clear_url_view(false);
        apply_package(bytes);
        append_bounded_url(m_url->approved_packages, pending.original_url);
        m_url->location = pending.original_url;
        return;
    }
    if (!supported_json(result.content_type)) throw PackageError("gateway did not return a Reader JSON document");
    ReaderDocument document = parse_reader_document(result.body);
    if (document.source_url.empty()) throw PackageError("Reader document has no source URL");
    validate_url(document.source_url, max_website_url_bytes);
    if (pending.kind == TransferKind::direct_document) document.source_url = pending.original_url;
    if ((document.mode == ReaderMode::interactive) != (pending.kind == TransferKind::interactive)) {
        throw PackageError("Reader response did not preserve the requested mode");
    }
    if (!document.session.has_value() && !m_url->session.empty()) document.session = m_url->session;
    const std::string address = document.source_url;
    const std::size_t size = result.body.size();
    install_reader_page(std::move(document), address);
    m_url->location = address;
    m_metrics.package_bytes = size;
}

void Browser::process_content_requests() {
    HttpResult result;
    while (m_platform.poll_content(result)) {
        try {
            if (m_url->pending_transfer && m_url->pending_transfer->id == result.id) {
                receive_transfer(std::move(result));
                continue;
            }
            const auto found = std::find_if(m_url->pending_images.begin(), m_url->pending_images.end(),
                [&](const UrlState::PendingImage& pending) { return pending.id == result.id; });
            if (found == m_url->pending_images.end()) continue;
            const std::size_t index = found->image;
            m_url->pending_images.erase(found);
            receive_image(index, std::move(result));
        } catch (const std::exception& error) {
            fail_url_load(error.what());
        }
    }
    m_metrics.active_requests += m_url->active_requests();
}

void Browser::enqueue_image(std::size_t index) {
    auto& image = m_url->images[index];
    if (m_url->pending_images.size() >= max_pending_images || image.queued || image.loaded) return;
    if (m_next_id > 16'777'215) throw ScriptError("image request identifier space is exhausted");
    std::string url = image.url;
    const std::size_t width = std::max(1, image.node->box.width);
    url += (url.find('?') == std::string::npos ? '?' : '&');
    url += "width=" + std::to_string(width) + "&y=" + std::to_string(image.y) + "&height=" + std::to_string(maximum_segment_height);
    validate_url(url, max_gateway_bytes);
    const uint32_t id = uint32_t(m_next_id++);
    m_platform.start_content(id, ContentRequest{.url = std::move(url), .body = {}});
    m_url->pending_images.push_back({id, index});
    image.queued = true;
}

void Browser::queue_visible_images() {
    if (!m_url->active() || m_url->images.empty()) return;
    if (m_layout_dirty) perform_layout();
    const Node* content = find_node("reader_content");
    if (content == nullptr) return;
    const Rect area = content->box;
    auto visible = [&](const UrlState::Image& image) {
        return image.node->box.y < area.y + area.height * 2 && image.node->box.y + image.node->box.height >= area.y - area.height;
    };
    for (auto& image : m_url->images) {
        if (image.loaded && !visible(image)) {
            const auto asset = std::find_if(m_package.assets.begin(), m_package.assets.end(),
                [&](const Asset& candidate) { return candidate.name == image.asset; });
            if (asset != m_package.assets.end()) {
                if (m_url->cached_image_bytes >= image.width * image.height * 2U) {
                    m_url->cached_image_bytes -= image.width * image.height * 2U;
                }
                *asset = Asset{image.asset, 1, 1, {to_rgb565(palette::surface)}};
                image.loaded = false;
                image.width = 0;
                image.height = 0;
                invalidate(*image.node);
            }
        }
    }
    const auto estimate = [](const UrlState::Image& image) {
        const uint32_t height = image.total_height > image.y
            ? std::min<uint32_t>(maximum_segment_height, image.total_height - image.y) : maximum_segment_height;
        return std::size_t(std::clamp(image.node->box.width, 1, 480)) * height * 2U;
    };
    std::size_t reserved = m_url->cached_image_bytes;
    for (const auto& pending : m_url->pending_images) reserved += estimate(m_url->images[pending.image]);
    for (std::size_t index = 0; index < m_url->images.size(); ++index) {
        const auto& image = m_url->images[index];
        if (!visible(image) || image.queued || image.loaded || image.failed) continue;
        if (reserved + estimate(image) > max_image_bytes) break;
        try {
            enqueue_image(index);
            reserved += estimate(image);
        } catch (const std::exception& error) {
            fail_url_load(error.what());
            return;
        }
    }
}

void Browser::receive_image(std::size_t index, HttpResult&& result) {
    if (index >= m_url->images.size()) throw PackageError("image response has no active destination");
    auto& image = m_url->images[index];
    const auto reject = [this, index](std::string_view reason) {
        auto& image = m_url->images[index];
        if (image.loaded) {
            const auto asset = std::find_if(m_package.assets.begin(), m_package.assets.end(),
                [&](const Asset& candidate) { return candidate.name == image.asset; });
            if (asset != m_package.assets.end()) *asset = Asset{image.asset, 1, 1, {to_rgb565(palette::surface)}};
            const std::size_t bytes = std::size_t(image.width) * std::size_t(image.height) * 2U;
            m_url->cached_image_bytes -= std::min(m_url->cached_image_bytes, bytes);
        }
        image.queued = false;
        image.loaded = false;
        image.failed = true;
        image.width = 0;
        image.height = 0;
        image.node->kind = NodeKind::text;
        image.node->text = "Image unavailable: " + std::string(reason);
        image.node->style.font_size = 12;
        image.node->style.font_weight = 400;
        image.node->style.foreground = palette::warning;
        image.node->style.height = Length{LengthKind::fixed,
            float(Canvas::wrapped_text_size(image.node->text, std::max(1, image.node->box.width), 12, 400).height)};
        m_platform.log("Reader image unavailable: " + std::string(reason));
        invalidate_layout();
    };
    image.queued = false;
    if (!result.succeeded()) {
        reject(result.error.empty() ? "HTTP status " + std::to_string(result.status) : result.error);
        return;
    }
    if (result.content_type != "application/vnd.microbrowser.rgb565") {
        reject("image was not converted to RGB565");
        return;
    }
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t total = 0;
    try {
        width = decimal_header(result.image_width, "width", 480);
        height = decimal_header(result.image_height, "height", maximum_segment_height);
        total = result.total_height.empty() ? height : decimal_header(result.total_height, "total height", maximum_scaled_image_height);
        const uint32_t offset = decimal_header(result.image_y, "segment offset", maximum_scaled_image_height, false);
        if (width == 0 || height == 0 || total < height || offset != image.y ||
            offset > total || height > total - offset || std::size_t(width) * height * 2U != result.body.size() ||
            (image.total_height != 0 && total != image.total_height)) {
            reject("image metadata does not match the RGB565 payload");
            return;
        }
    } catch (const PackageError& error) {
        reject(error.what());
        return;
    }
    image.total_height = total;
    auto asset = std::find_if(m_package.assets.begin(), m_package.assets.end(),
        [&](const Asset& candidate) { return candidate.name == image.asset; });
    if (asset == m_package.assets.end()) throw PackageError("image asset is missing from its document");
    Asset decoded{image.asset, uint16_t(width), uint16_t(height), std::vector<uint16_t>(std::size_t(width) * height)};
    std::memcpy(decoded.pixels.data(), result.body.data(), result.body.size());
    m_url->cached_image_bytes += result.body.size();
    *asset = std::move(decoded);
    image.loaded = true;
    image.width = uint16_t(width);
    image.height = uint16_t(height);
    image.node->style.height = Length{LengthKind::fixed, float(height)};
    invalidate_layout();
    if (image.y == 0 && total > height && !image.expanded) {
        Node* parent = find_image_parent(m_package.pages[0].root, image.asset);
        if (parent == nullptr) throw PackageError("image parent container is missing");
        Node* first = find_image(*parent, image.asset);
        if (first == nullptr) throw PackageError("image node is missing");
        auto target = std::find_if(parent->children.begin(), parent->children.end(),
            [&](const auto& child) { return child.get() == first; });
        if (target == parent->children.end()) throw PackageError("image node is missing from its container");
        const Style source_style = first->style;
        const std::string asset = image.asset;
        const std::string url = image.url;
        const std::size_t block_index = image.block_index;
        const std::size_t remaining = (total - height + maximum_segment_height - 1) / maximum_segment_height;
        if (parent->children.size() + remaining > 64 || m_metrics.node_count + remaining > 2048) {
            reject("image exceeds the document element budget");
            return;
        }
        auto segments = std::make_unique<Node>();
        segments->kind = NodeKind::column;
        segments->style = source_style;
        segments->style.height = Length{LengthKind::fixed, float(total)};
        segments->style.gap = 0;
        segments->children.push_back(std::move(*target));
        Node* container = segments.get();
        *target = std::move(segments);
        image.expanded = true;
        for (uint32_t y = height; y < total; y += maximum_segment_height) {
            auto segment = std::make_unique<Node>();
            segment->kind = NodeKind::image;
            segment->style = source_style;
            segment->source = asset + ":" + std::to_string(y);
            segment->style.height = Length{LengthKind::fixed, float(std::min<uint32_t>(maximum_segment_height, total - y))};
            Node* pointer = segment.get();
            container->children.push_back(std::move(segment));
            m_package.assets.push_back(Asset{pointer->source, 1, 1, {to_rgb565(palette::surface)}});
            m_url->images.push_back({pointer->source, url, pointer, block_index, y,
                                     uint16_t(width), 0, total, false, false, false, false});
            ++m_metrics.node_count;
        }
    }
    queue_visible_images();
}

} // namespace mb
