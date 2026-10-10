#pragma once
#include "vertex.hpp"
#include <cstdint>
#include <string_view>
#include <vector>
namespace engine::ui {
struct Point {
    float x = 0, y = 0;
};
struct Color {
    float r = 1, g = 1, b = 1, a = 1;
};
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};
enum class FontFace { Sans, SansBold, Serif, SerifItalic, Mono };
struct TextStyle {
    FontFace face = FontFace::Sans;
    float size = 14;
    Color color{};
    float tracking = 0;
    float lineHeight = 1.35f;
};
struct FontMetrics {
    float ascent = 0, descent = 0, lineGap = 0;
};
struct FontAtlas {
    int width = 0, height = 0;
    std::vector<unsigned char> rgba;
};
// Loading/rasterization is portable CPU work. Upload remains with the GPU owner.
bool buildFontAtlas(std::string_view directory, FontAtlas& result);
bool fontsReady();
bool hasGlyph(FontFace face, char32_t codepoint);
FontMetrics fontMetrics(const TextStyle& style);
float measureText(std::string_view text, const TextStyle& style);

class Canvas {
    float width_ = 1, height_ = 1;
    std::vector<Vertex>* output_ = nullptr;
    std::vector<Rect> clips_;
    struct Hit {
        uint32_t id;
        Rect rect;
    };
    std::vector<Hit> hits_;
    void triangle(Point a, Point b, Point c, Color ca, Color cb, Color cc);
    void texturedRect(Rect rect, Rect uv, Color color, float mode);

  public:
    void begin(float logicalWidth, float logicalHeight, std::vector<Vertex>& output);
    float width() const {
        return width_;
    }
    float height() const {
        return height_;
    }
    void clip(Rect rect);
    void resetClip();
    void popClip();
    void rect(Rect rect, Color color);
    void triangle(Point a, Point b, Point c, Color color) {
        triangle(a, b, c, color, color, color);
    }
    void rect(float x, float y, float w, float h, Color color) {
        rect({x, y, w, h}, color);
    }
    void gradient(Rect rect, Color topLeft, Color topRight, Color bottomRight, Color bottomLeft);
    void gradientRect(float x, float y, float w, float h, Color left, Color right) {
        gradient({x, y, w, h}, left, right, right, left);
    }
    void roundedRect(Rect rect, float radius, Color color);
    void line(float x1, float y1, float x2, float y2, float width, Color color);
    void circle(float cx, float cy, float radius, Color color, int parts = 32);
    void arc(float cx, float cy, float radius, float from, float to, float width, Color color,
             int parts = 48);
    void path(const std::vector<Point>& points, float width, Color color, bool closed = false);
    // UV coordinates reference an uploaded atlas region; image sampling preserves RGB.
    void image(Rect destination, Rect atlasUV, Color tint = {});
    void text(float x, float y, std::string_view value, const TextStyle& style);
    float textWidth(std::string_view value, const TextStyle& style) const {
        return measureText(value, style);
    }
    void textCentered(float centerX, float y, std::string_view value, const TextStyle& style);
    void textRight(float rightX, float y, std::string_view value, const TextStyle& style);
    float wrappedText(Rect area, std::string_view value, const TextStyle& style);
    void hitbox(uint32_t id, Rect rect);
    uint32_t hitTest(float x, float y) const;
};

enum class Axis { Row, Column };
enum class Align { Start, Center, End, Stretch };
struct Insets {
    float left = 0, top = 0, right = 0, bottom = 0;
};
struct LayoutStyle {
    Axis axis = Axis::Column;
    Align cross = Align::Start;
    float gap = 0, grow = 0;
    Insets padding{};
    Point minimum{}, preferred{};
};
// Small retained layout tree: measure/arrange once, then use each box for paint/input.
class Layout {
  public:
    LayoutStyle style{};
    Rect bounds{};
    std::vector<Layout> children;
    Point measure() const;
    void arrange(Rect area);
};
} // namespace engine::ui
