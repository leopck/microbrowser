#include "reader.h"

#include "fonts.h"
#include "microbrowser/canvas.h"
#include "microbrowser/package.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace mb {
namespace {

constexpr std::size_t max_remote_nodes = 2'048;
constexpr std::size_t max_section_children = 60;
constexpr std::size_t max_link_label_bytes = 192;
constexpr int toolbar_height = 42;
constexpr int footer_height = 18;
constexpr int content_padding = 12;
constexpr int default_image_height = 240;
constexpr int max_image_dimension = 16'384;
constexpr std::string_view font_notice =
    "Some characters outside the bundled Inter subset are shown as '?'.";
constexpr std::string_view remote_privacy_notice =
    "Remote gateway sees requested URLs. No passwords, payments or verification.";

Length fixed(int value) {
    return {LengthKind::fixed, static_cast<float>(value)};
}

Length percent(int value) {
    return {LengthKind::percent, static_cast<float>(value)};
}

Length grow() {
    return {LengthKind::grow, 0.0F};
}

int usable_content_width(int viewport_width) {
    const int64_t width = static_cast<int64_t>(viewport_width) - 2 * content_padding;
    return static_cast<int>(std::clamp<int64_t>(width, 1, std::numeric_limits<int>::max()));
}

void validate_utf8(std::string_view text, std::string_view description) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const Utf8Character character = next_utf8(text, offset);
        if (character.codepoint == '?' &&
            static_cast<unsigned char>(text[offset]) != static_cast<unsigned char>('?')) {
            throw PackageError(std::string(description) + " contains malformed UTF-8 at byte " +
                               std::to_string(offset));
        }
        offset += character.length;
    }
}

bool needs_font_fallback(std::string_view text) {
    for (std::size_t offset = 0; offset < text.size();) {
        const Utf8Character character = next_utf8(text, offset);
        offset += character.length;
        const uint32_t codepoint = character.codepoint;
        if (codepoint == '\n' || codepoint == '\t' || codepoint == '?') {
            continue;
        }
        if (fonts::glyph_index(codepoint) == fonts::glyph_index('?')) {
            return true;
        }
    }
    return false;
}

std::string_view utf8_prefix(std::string_view text, std::size_t maximum_bytes) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const std::size_t next = offset + next_utf8(text, offset).length;
        if (next > maximum_bytes) {
            break;
        }
        offset = next;
    }
    return text.substr(0, offset);
}

std::size_t previous_codepoint(std::string_view text, std::size_t offset) {
    if (offset == 0) {
        return 0;
    }
    std::size_t previous = 0;
    for (std::size_t current = 0; current < offset;) {
        previous = current;
        current += next_utf8(text, current).length;
    }
    return previous;
}

std::string button_label(std::string_view text, int available_width, int padding) {
    const int text_width_limit = std::max(0, available_width - 2 * padding);
    const std::string_view prefix = utf8_prefix(text, max_link_label_bytes);
    if (Canvas::text_width(prefix, 14, 600) <= text_width_limit) {
        return std::string(prefix);
    }

    std::size_t end = prefix.size();
    while (end > 0) {
        end = previous_codepoint(prefix, end);
        std::string candidate(prefix.substr(0, end));
        candidate += "...";
        if (Canvas::text_width(candidate, 14, 600) <= text_width_limit) {
            return candidate;
        }
    }
    return {};
}

class ReaderPageBuilder {
public:
    ReaderPageBuilder(const ReaderDocument& document, std::string_view address, Size viewport)
        : document_(document), address_(address),
          content_width_(usable_content_width(viewport.width)) {
        result_.states.push_back(StateField{"address", Value::from_string(std::string(address))});
        result_.links.reserve(8 + document.blocks.size());
    }

    RemotePage build() {
        validate_strings();
        configure_root();
        Node& toolbar = append_child(result_.page.root, NodeKind::row);
        build_toolbar(toolbar);
        Node& body = append_child(result_.page.root, NodeKind::view);
        body.style.width = percent(100);
        body.style.height = grow();
        content_ = &append_child(body, NodeKind::column);
        configure_content(*content_);
        build_content();
        build_footer();
        return std::move(result_);
    }

private:
    const ReaderDocument& document_;
    std::string_view address_;
    int content_width_;
    RemotePage result_;
    Node* content_ = nullptr;
    Node* section_ = nullptr;
    std::size_t section_children_ = 0;
    std::size_t node_count_ = 1;

    void validate_strings() const {
        validate_utf8(address_, "reader address");
        validate_utf8(document_.title, "reader title");
        validate_utf8(document_.source_url, "reader source URL");
        if (document_.source_url.empty()) {
            throw PackageError("reader source URL must be nonempty");
        }
        validate_optional_utf8(document_.previous_url, "reader previous URL");
        validate_optional_utf8(document_.next_url, "reader next URL");
        validate_optional_utf8(document_.session, "reader session");
        validate_optional_utf8(document_.unsupported_reason, "reader unsupported reason");
        for (const ReaderBlock& block : document_.blocks) {
            validate_utf8(block.text, "reader block text");
            validate_utf8(block.url, "reader block URL");
        }
    }

    static void validate_optional_utf8(const std::optional<std::string>& value,
                                       std::string_view description) {
        if (value) {
            validate_utf8(*value, description);
        }
    }

    Node& append_child(Node& parent, NodeKind kind) {
        if (node_count_ == max_remote_nodes) {
            throw PackageError("reader page exceeds 2048 nodes");
        }
        auto child = std::make_unique<Node>();
        child->kind = kind;
        Node* result = child.get();
        parent.children.push_back(std::move(child));
        ++node_count_;
        return *result;
    }

    void attach(Node& node, RemoteAction action) {
        result_.links.push_back(RemoteLink{&node, action, {}});
    }

    void attach(Node& node, RemoteAction action, const std::string& url) {
        result_.links.push_back(RemoteLink{&node, action, url});
    }

    void configure_root() {
        result_.page.name = document_.mode == ReaderMode::interactive ? "interactive" : "reader";
        Node& root = result_.page.root;
        root.kind = NodeKind::column;
        root.style.width = percent(100);
        root.style.height = percent(100);
        root.style.background = palette::background;
    }

    void build_toolbar(Node& toolbar) {
        const bool compact = content_width_ < 336;
        toolbar.style.width = percent(100);
        toolbar.style.height = fixed(toolbar_height);
        toolbar.style.padding = 4;
        toolbar.style.gap = compact ? 3 : 6;
        toolbar.style.background = palette::surface;
        toolbar.style.border = palette::border;

        Node& back = append_child(toolbar, NodeKind::button);
        configure_toolbar_button(back, "<", 34);
        attach(back, RemoteAction::go_back);
        Node& home = append_child(toolbar, NodeKind::button);
        configure_toolbar_button(home, "Home", compact ? 44 : 56);
        attach(home, RemoteAction::go_home);

        Node& address_host = append_child(toolbar, NodeKind::view);
        address_host.style.width = grow();
        address_host.style.height = fixed(34);
        Node& input = append_child(address_host, NodeKind::input);
        input.style.absolute = true;
        input.style.width = grow();
        input.style.height = grow();
        input.style.border = palette::border;
        input.style.radius = 4;
        input.placeholder = "Website URL";
        input.binding = 0;
        input.max_length = 2'047;
        attach(input, RemoteAction::submit_address);

        Node& go = append_child(toolbar, NodeKind::button);
        configure_toolbar_button(go, "Go", compact ? 38 : 44);
        attach(go, RemoteAction::submit_address);
    }

    static void configure_toolbar_button(Node& node, std::string_view label, int width) {
        node.text = label;
        node.style.width = fixed(width);
        node.style.height = fixed(34);
        node.style.padding = 3;
        node.style.font_size = 14;
        node.style.font_weight = 600;
        node.style.foreground = palette::ink;
        node.style.background = palette::surface;
        node.style.border = palette::border;
        node.style.radius = 4;
    }

    void configure_content(Node& content) const {
        content.id = "reader_content";
        content.style.absolute = true;
        content.style.width = percent(100);
        content.style.height = grow();
        content.style.scroll = true;
        content.style.gap = 8;
        content.style.padding = content_padding;
    }

    Node& content_parent() {
        if (section_ == nullptr || section_children_ == max_section_children) {
            section_ = &append_child(*content_, NodeKind::column);
            section_->style.width = percent(100);
            section_->style.gap = 8;
            section_children_ = 0;
        }
        return *section_;
    }

    Node& append_content_node(NodeKind kind) {
        Node& node = append_child(content_parent(), kind);
        ++section_children_;
        return node;
    }

    Node& add_text(Node& parent, std::string_view text, int font_size, int font_weight,
                   Color foreground, int padding = 0) {
        Node& node = append_child(parent, NodeKind::text);
        node.text = text;
        node.style.width = percent(100);
        node.style.padding = padding;
        node.style.font_size = font_size;
        node.style.font_weight = font_weight;
        node.style.foreground = foreground;
        const int wrapping_width = std::max(1, content_width_ - 2 * padding);
        const Size measured = Canvas::wrapped_text_size(text, wrapping_width, font_size, font_weight);
        node.style.height = fixed(measured.height + 2 * padding);
        return node;
    }

    Node& add_content_text(std::string_view text, int font_size, int font_weight,
                           Color foreground) {
        Node& parent = content_parent();
        Node& node = add_text(parent, text, font_size, font_weight, foreground);
        ++section_children_;
        return node;
    }

    Node& add_content_button(std::string_view text, const std::string& url, RemoteAction action) {
        Node& node = append_content_node(NodeKind::button);
        node.text = button_label(text, content_width_, 8);
        node.style.width = percent(100);
        node.style.height = fixed(42);
        node.style.padding = 8;
        node.style.font_size = 14;
        node.style.font_weight = 600;
        node.style.foreground = palette::blue;
        node.style.background = palette::surface;
        node.style.border = palette::border;
        node.style.radius = 4;
        attach(node, action, url);
        return node;
    }

    bool needs_font_notice() const {
        if (needs_font_fallback(address_) || needs_font_fallback(document_.title)) {
            return true;
        }
        if (document_.unsupported_reason && needs_font_fallback(*document_.unsupported_reason)) {
            return true;
        }
        for (const ReaderBlock& block : document_.blocks) {
            if (block.kind == ReaderBlockKind::image) {
                continue;
            }
            if (needs_font_fallback(block.text)) {
                return true;
            }
            if (block.kind == ReaderBlockKind::link && block.text.empty() &&
                needs_font_fallback(block.url)) {
                return true;
            }
        }
        return false;
    }

    void build_content() {
        bool heading_is_title = false;
        for (const auto& block : document_.blocks) {
            if (block.kind == ReaderBlockKind::heading && block.text == document_.title) {
                heading_is_title = true;
                break;
            }
        }
        if (!document_.title.empty() && !heading_is_title) {
            add_text(*content_, document_.title, 20, 600, palette::ink, 4);
        }
        if (needs_font_notice()) {
            add_text(*content_, font_notice, 12, 400, palette::warning, 4);
        }
        if (document_.kind == ReaderKind::unsupported || document_.kind == ReaderKind::native_package_confirmation) {
            build_unsupported_content();
        }
        for (std::size_t index = 0; index < document_.blocks.size(); ++index) {
            build_block(document_.blocks[index], index);
        }
        if (document_.previous_url) {
            add_content_button("Previous", *document_.previous_url, RemoteAction::reader_url);
        }
        if (document_.next_url) {
            add_content_button("Next", *document_.next_url, RemoteAction::reader_url);
        }
    }

    void build_unsupported_content() {
        if (document_.unsupported_reason && !document_.unsupported_reason->empty()) {
            add_content_text(*document_.unsupported_reason, 14, 400, palette::muted);
        }
        if (document_.interactive_available) {
            Node& open = add_content_button("Open interactive website", document_.source_url,
                                            RemoteAction::interactive_url);
            open.style.background = palette::blue;
            open.style.foreground = palette::white;
            open.style.border = palette::blue;
            add_content_text(remote_privacy_notice, 14, 400, palette::muted);
        } else {
            add_content_text("Interactive mode is currently unavailable for this website.",
                             12, 400, palette::warning);
        }
    }

    void build_block(const ReaderBlock& block, std::size_t index) {
        switch (block.kind) {
        case ReaderBlockKind::heading:
            add_content_text(block.text, 20, 600, palette::ink);
            return;
        case ReaderBlockKind::paragraph:
            add_content_text(block.text, 14, 400, palette::ink);
            return;
        case ReaderBlockKind::notice:
            add_content_text(block.text, 12, 400, palette::muted);
            return;
        case ReaderBlockKind::link:
            if ((document_.next_url && block.url == *document_.next_url) ||
                (document_.previous_url && block.url == *document_.previous_url)) {
                return;
            }
            add_content_button(block.text.empty() ? std::string_view(block.url)
                                                  : std::string_view(block.text),
                               block.url,
                               document_.kind == ReaderKind::native_package_confirmation &&
                               block.url == document_.source_url ?
                                   RemoteAction::run_native_package : RemoteAction::reader_url);
            return;
        case ReaderBlockKind::image:
            build_image(block, index);
            return;
        }
        throw PackageError("reader block has an invalid kind");
    }

    void build_image(const ReaderBlock& block, std::size_t index) {
        Node& node = append_content_node(NodeKind::image);
        node.source = "remote-block:" + std::to_string(index);
        node.style.width = percent(100);
        node.style.height = fixed(image_height(block));
    }

    int image_height(const ReaderBlock& block) const {
        if (!block.width || !block.height || *block.width <= 0 || *block.height <= 0 ||
            *block.width > max_image_dimension || *block.height > max_image_dimension) {
            return default_image_height;
        }
        const int64_t numerator = static_cast<int64_t>(*block.height) * content_width_ +
                                  *block.width / 2;
        const int64_t scaled = numerator / *block.width;
        return static_cast<int>(std::clamp<int64_t>(scaled, 1, max_image_dimension));
    }

    void build_footer() {
        Node& footer = append_child(result_.page.root, NodeKind::text);
        const bool interactive = document_.mode == ReaderMode::interactive;
        footer.text = interactive ? "Interactive | gateway processed"
                                  : "Reader | optimized text/images";
        footer.style.width = percent(100);
        footer.style.height = fixed(footer_height);
        footer.style.font_size = 12;
        footer.style.foreground = interactive ? palette::blue : palette::muted;
        footer.style.background = palette::surface;
    }
};

} // namespace

RemotePage make_reader_page(const ReaderDocument& document, std::string_view address,
                            Size viewport) {
    return ReaderPageBuilder(document, address, viewport).build();
}

} // namespace mb
