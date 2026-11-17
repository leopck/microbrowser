#pragma once

#include "microbrowser/browser.h"

namespace mb {

struct Layout {
    Browser& browser;

    void run();

private:
    Size measure(const Node& node, Size available);
    void place(Node& node, Rect area);
    void place_children(Node& node);
    void place_stack(Node& node, Rect content);
    int resolve(Length length, int available, int fallback) const;
    Size bound_size(const Node& node, Size available, Size measured);
    void text_size(const Node& node, std::string_view content, Size available,
                   int& width, int& height) const;
};

} // namespace mb
