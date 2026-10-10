#pragma once
namespace engine::ui {
// GPU-independent paint data; positions are clip-space, all layout uses logical pixels.
struct Vertex {
    float x, y, u, v, r, g, b, a;
    float mode = 0;
};
} // namespace engine::ui
namespace engine {
using UiVertex = engine::ui::Vertex;
}
