#pragma once

#include "microbrowser/browser.h"
#include "microbrowser/canvas.h"

namespace mb {

Rect visual_bounds(const Node& node);

struct Painter {
    Browser& browser;
    Canvas canvas;

    void run(Rect clip);
    void overlay(Rect clip);
    void draw_children(const Node& node, uint8_t opacity, Rect clip);

private:
    void draw(const Node& node, uint8_t parent_opacity, Rect clip);
    void scrollbar(const Node& node, Rect area, uint8_t opacity, Rect clip);
    void button(const Node& node, Rect area, uint8_t opacity, Rect clip);
    void input(const Node& node, Rect area, uint8_t opacity, Rect clip);
    void text(const Node& node, Rect area, uint8_t opacity, Rect clip);
    void content_box(const Node& node, Rect area, float number, Color color, uint8_t opacity, Rect clip);
    void chart(const Node& node, Rect area, uint8_t opacity, Rect clip);
    void keyboard(Rect clip);
};

} // namespace mb
