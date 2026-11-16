#include "layout.h"
#include "input.h"
#include "microbrowser/canvas.h"

#include <algorithm>
#include <array>

namespace mb {
namespace {

bool horizontal(const Node& node) { return node.kind == NodeKind::row; }
bool stacks(const Node& node) { return node.kind == NodeKind::stack; }
bool container(const Node& node) {
    return stacks(node) || horizontal(node) || node.kind == NodeKind::column || node.kind == NodeKind::view;
}
bool grows(const Node& node, bool along_x) {
    return node.kind == NodeKind::spacer ||
           (along_x ? node.style.width.kind : node.style.height.kind) == LengthKind::grow;
}

struct ChildLayout {
    Node* node = nullptr;
    Size measurement{};
};

} // namespace

int Layout::resolve(Length length, int available, int fallback) const {
    switch (length.kind) {
    case LengthKind::fixed: return std::max(0, int(length.value));
    case LengthKind::percent: return std::max(0, int(available * length.value / 100.0F));
    case LengthKind::automatic: return fallback;
    case LengthKind::grow: return std::max(0, available);
    }
    return fallback;
}

Size Layout::bound_size(const Node& node, Size available, Size measured) {
    const int width = node.style.width.kind == LengthKind::grow ? measured.width
        : resolve(node.style.width, available.width, measured.width);
    const int height = node.style.height.kind == LengthKind::grow ? measured.height
        : resolve(node.style.height, available.height, measured.height);
    return {width, height};
}

void Layout::text_size(const Node& node, std::string_view content, int& width, int& height) const {
    width = Canvas::text_width(content, node.style.font_size) + node.style.padding * 2;
    height = Canvas::text_height(node.style.font_size) + std::max(12, node.style.padding * 2);
}

Size Layout::measure(const Node& node, Size available) {
    Size measured{};
    if (node.kind == NodeKind::text || node.kind == NodeKind::button) {
        std::array<char, 256> buffer{};
        std::string_view content = node.text;
        if (node.binding != no_index) content = format_value(browser.m_state[node.binding], node.format, buffer);
        if (content.empty() && node.kind == NodeKind::button) content = " ";
        text_size(node, content, measured.width, measured.height);
    } else if (node.kind == NodeKind::input) {
        measured = {180 + node.style.padding * 2, 32 + node.style.padding * 2};
    } else if (node.kind == NodeKind::slider) {
        measured = {160, 26};
    } else if (node.kind == NodeKind::progress) {
        measured = {180, 16};
    } else if (node.kind == NodeKind::chart) {
        measured = {220, 90};
    } else if (node.kind == NodeKind::image) {
        const auto asset = std::find_if(browser.m_package.assets.begin(), browser.m_package.assets.end(),
            [&](const Asset& candidate) { return candidate.name == node.source; });
        if (asset != browser.m_package.assets.end()) measured = {asset->width, asset->height};
    } else if (container(node)) {
        const bool along_x = horizontal(node);
        int main = 0;
        int cross = 0;
        std::size_t child_count = 0;
        for (const auto& owned : node.children) {
            const Node& child = *owned;
            if (child.style.absolute) continue;
            const Size child_size = measure(child, available);
            ++child_count;
            if (stacks(node)) {
                main = std::max(main, along_x ? child_size.width : child_size.height);
                cross = std::max(cross, along_x ? child_size.height : child_size.width);
            } else if (along_x) {
                main += child_size.width;
                cross = std::max(cross, child_size.height);
            } else {
                main += child_size.height;
                cross = std::max(cross, child_size.width);
            }
        }
        if (!stacks(node) && child_count > 1) main += (int(child_count) - 1) * node.style.gap;
        measured = along_x ? Size{main + 2 * node.style.padding, cross + 2 * node.style.padding}
                           : Size{cross + 2 * node.style.padding, main + 2 * node.style.padding};
    }
    return bound_size(node, available, measured);
}

void Layout::place(Node& node, Rect area) {
    node.box = area;
    if (container(node)) place_children(node);
}

void Layout::place_stack(Node& node, Rect content) {
    for (auto& owned : node.children) {
        Node& child = *owned;
        const Size measurement = measure(child, {content.width, content.height});
        if (child.style.absolute) {
            const int width = resolve(child.style.width, content.width, measurement.width);
            const int height = resolve(child.style.height, content.height, measurement.height);
            place(child, {content.x + child.style.x, content.y + child.style.y, width, height});
        } else {
            const Size size = bound_size(child, {content.width, content.height}, {content.width, content.height});
            place(child, {content.x, content.y, size.width, size.height});
        }
    }
}

void Layout::place_children(Node& node) {
    const Rect content{node.box.x + node.style.padding, node.box.y + node.style.padding,
                       std::max(0, node.box.width - 2 * node.style.padding),
                       std::max(0, node.box.height - 2 * node.style.padding)};
    if (stacks(node)) {
        place_stack(node, content);
        node.scroll_max = 0;
        return;
    }

    const bool along_x = horizontal(node);
    const int available = along_x ? content.width : content.height;
    std::array<ChildLayout, 64> children{};
    std::array<ChildLayout, 64> absolute_children{};
    std::size_t count = 0;
    std::size_t absolute_count = 0;
    int grow_count = 0;
    int occupied = 0;
    for (auto& owned : node.children) {
        Node& child = *owned;
        ChildLayout item{&child, measure(child, {content.width, content.height})};
        if (child.style.absolute) {
            if (absolute_count == absolute_children.size()) throw PackageError("layout allows at most 64 children per container");
            absolute_children[absolute_count++] = item;
            continue;
        }
        if (count == children.size()) throw PackageError("layout allows at most 64 children per container");
        children[count++] = item;
        if (grows(child, along_x)) ++grow_count;
        else occupied += along_x ? item.measurement.width : item.measurement.height;
    }
    const int normal_gaps = count > 1 ? (int(count) - 1) * node.style.gap : 0;
    const int free_space = node.style.scroll ? 0 : std::max(0, available - occupied - normal_gaps);
    int remaining = free_space;
    int between = node.style.gap;
    int cursor = along_x ? content.x : content.y;
    if (grow_count == 0 && !node.style.scroll && count > 1 && node.style.justify == Justify::space_between) {
        between += free_space / (int(count) - 1);
    } else if (node.style.justify == Justify::center) {
        cursor += free_space / 2;
    } else if (node.style.justify == Justify::end_position) {
        cursor += free_space;
    }

    node.scroll = std::clamp(node.scroll, 0.0F, node.scroll_max);
    for (std::size_t index = 0; index < count; ++index) {
        Node& child = *children[index].node;
        const Size measurement = children[index].measurement;
        int main = 0;
        if (grows(child, along_x)) {
            main = remaining / grow_count;
            remaining -= main;
            --grow_count;
        } else {
            main = along_x ? measurement.width : measurement.height;
        }
        const int cross_available = along_x ? content.height : content.width;
        int cross = along_x ? measurement.height : measurement.width;
        if (node.style.align == Align::stretch) cross = cross_available;
        int cross_position = along_x ? content.y : content.x;
        if (node.style.align == Align::center) cross_position += (cross_available - cross) / 2;
        if (node.style.align == Align::end_position) cross_position += cross_available - cross;
        if (along_x) {
            place(child, {cursor, cross_position, main, cross});
        } else {
            place(child, {cross_position, cursor - int(node.scroll), cross, main});
        }
        cursor += main + between;
    }
    for (std::size_t index = 0; index < absolute_count; ++index) {
        Node& child = *absolute_children[index].node;
        const Size size = absolute_children[index].measurement;
        const int width = resolve(child.style.width, content.width, size.width);
        const int height = resolve(child.style.height, content.height, size.height);
        place(child, {content.x + child.style.x, content.y + child.style.y, width, height});
    }
    node.scroll_max = node.style.scroll
        ? std::max(0, cursor - node.style.gap - (along_x ? content.x : content.y) - available)
        : 0.0F;
}

void Layout::run() {
    if (browser.m_page == nullptr) return;
    const int height = browser.m_show_keyboard ? browser.m_size.height - keyboard_height(browser.m_size) : browser.m_size.height;
    place(browser.m_page->root, {0, 0, browser.m_size.width, std::max(0, height)});
}

} // namespace mb
