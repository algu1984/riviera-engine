#include "engine/ui/canvas.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <string>
#include <unordered_map>
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_truetype.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace engine::ui {
namespace {
constexpr float pi = 3.14159265358979323846f;
constexpr int atlasWidth = 4096, atlasHeight = 2048;
struct Face {
    std::vector<unsigned char> bytes;
    stbtt_fontinfo info{};
    std::vector<stbtt_packedchar> glyphs;
    float rasterSize = 80, ascent = 0, descent = 0, gap = 0;
};
std::array<Face, 5> faces;
std::vector<int> codepoints;
std::unordered_map<uint32_t, size_t> glyphIndex;
bool ready = false;
uint32_t decode(std::string_view str, size_t& at) {
    if (at >= str.size())
        return 0;
    unsigned char c = static_cast<unsigned char>(str[at++]);
    if (c < 128)
        return c;
    int count = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc2 ? 1 : 0;
    uint32_t value = c & ((1u << (6 - count)) - 1u);
    if (!count || at + count > str.size())
        return '?';
    for (int i = 0; i < count; ++i) {
        unsigned char next = static_cast<unsigned char>(str[at]);
        if ((next & 0xc0) != 0x80)
            return '?';
        ++at;
        value = (value << 6) | (next & 0x3f);
    }
    return value;
}
Face& face(const TextStyle& style) {
    return faces[static_cast<size_t>(style.face)];
}
size_t index(uint32_t c) {
    auto found = glyphIndex.find(c);
    return found == glyphIndex.end() ? glyphIndex.at('?') : found->second;
}
Rect intersection(Rect a, Rect b) {
    float x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::max(0.f, std::min(a.x + a.w, b.x + b.w) - x),
            std::max(0.f, std::min(a.y + a.h, b.y + b.h) - y)};
}
struct PaintPoint {
    Point p;
    Color c;
};
PaintPoint interpolate(PaintPoint a, PaintPoint b, float t) {
    return {{a.p.x + (b.p.x - a.p.x) * t, a.p.y + (b.p.y - a.p.y) * t},
            {a.c.r + (b.c.r - a.c.r) * t, a.c.g + (b.c.g - a.c.g) * t, a.c.b + (b.c.b - a.c.b) * t,
             a.c.a + (b.c.a - a.c.a) * t}};
}
} // namespace

bool buildFontAtlas(std::string_view directory, FontAtlas& result) {
    ready = false;
    codepoints.clear();
    glyphIndex.clear();
    for (int c = 32; c < 256; ++c)
        if (c < 127 || c >= 160)
            codepoints.push_back(c);
    for (int c : {0x2013, 0x2014, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2026, 0x2032, 0x2190,
                  0x2191, 0x2192, 0x2193, 0x2197, 0x21bb, 0x25b6, 0x25c0, 0x25e6})
        codepoints.push_back(c);
    for (size_t i = 0; i < codepoints.size(); ++i)
        glyphIndex[codepoints[i]] = i;
    const char* files[] = {"Arimo-wght.ttf", "Arimo-Bold.ttf", "Gelasio-wght.ttf",
                           "Gelasio-Italic-wght.ttf", "Cousine-Regular.ttf"};
    std::vector<unsigned char> mask(atlasWidth * atlasHeight);
    stbtt_pack_context pack{};
    // Twelve source pixels of gutter keep adjacent glyphs out of mip levels 0..4.
    if (!stbtt_PackBegin(&pack, mask.data(), atlasWidth, atlasHeight, 0, 12, nullptr))
        return false;
    bool ok = true;
    for (size_t i = 0; i < faces.size(); ++i) {
        auto& f = faces[i];
        std::ifstream file(std::string(directory) + "/" + files[i],
                           std::ios::binary | std::ios::ate);
        auto length = file ? file.tellg() : std::streampos(-1);
        if (length <= 0) {
            ok = false;
            break;
        }
        f.bytes.resize(static_cast<size_t>(length));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(f.bytes.data()), length);
        int offset = stbtt_GetFontOffsetForIndex(f.bytes.data(), 0);
        if (!file || offset < 0 || !stbtt_InitFont(&f.info, f.bytes.data(), offset)) {
            ok = false;
            break;
        }
        int a, d, g;
        stbtt_GetFontVMetrics(&f.info, &a, &d, &g);
        // Text size follows CSS em sizing rather than stb's ascent-to-descent pixel height.
        float emScale = stbtt_ScaleForMappingEmToPixels(&f.info, 1.f);
        f.ascent = a * emScale;
        f.descent = d * emScale;
        f.gap = g * emScale;
        f.rasterSize = i == 2 || i == 3 ? 112.f : 80.f;
        f.glyphs.resize(codepoints.size());
        stbtt_pack_range range{};
        range.font_size = STBTT_POINT_SIZE(f.rasterSize);
        range.array_of_unicode_codepoints = codepoints.data();
        range.num_chars = int(codepoints.size());
        range.chardata_for_range = f.glyphs.data();
        stbtt_PackSetOversampling(&pack, 1, 1);
        if (!stbtt_PackFontRanges(&pack, f.bytes.data(), 0, &range, 1)) {
            ok = false;
            break;
        }
        // Some display faces do not contain every symbol; use the real '?' glyph
        // rather than a font-dependent missing-box glyph. This is not script shaping.
        for (size_t glyph = 0; glyph < codepoints.size(); ++glyph)
            if (!stbtt_FindGlyphIndex(&f.info, codepoints[glyph]))
                f.glyphs[glyph] = f.glyphs[glyphIndex.at('?')];
    }
    stbtt_PackEnd(&pack);
    if (!ok)
        return false;
    result.width = atlasWidth;
    result.height = atlasHeight;
    result.rgba.resize(mask.size() * 4);
    for (size_t i = 0; i < mask.size(); ++i) {
        result.rgba[i * 4] = result.rgba[i * 4 + 1] = result.rgba[i * 4 + 2] = 255;
        result.rgba[i * 4 + 3] = mask[i];
    }
    ready = true;
    return true;
}
bool fontsReady() {
    return ready;
}
bool hasGlyph(FontFace role, char32_t codepoint) {
    return ready &&
           stbtt_FindGlyphIndex(&faces[static_cast<size_t>(role)].info, int(codepoint)) != 0;
}
FontMetrics fontMetrics(const TextStyle& style) {
    if (!ready)
        return {style.size * .8f, -style.size * .2f, 0};
    auto& f = face(style);
    return {f.ascent * style.size, f.descent * style.size, f.gap * style.size};
}
float measureText(std::string_view str, const TextStyle& style) {
    if (!ready)
        return str.size() * style.size * .55f;
    auto& f = face(style);
    float width = 0, maxWidth = 0, scale = style.size / f.rasterSize;
    uint32_t previous = 0;
    size_t count = 0;
    for (size_t at = 0; at < str.size();) {
        uint32_t c = decode(str, at);
        if (c == '\n') {
            maxWidth = std::max(maxWidth, width);
            width = 0;
            previous = 0;
            count = 0;
            continue;
        }
        if (count++)
            width += style.tracking;
        if (previous)
            width += stbtt_GetCodepointKernAdvance(&f.info, int(previous), int(c)) *
                     stbtt_ScaleForMappingEmToPixels(&f.info, style.size);
        width += f.glyphs[index(c)].xadvance * scale;
        previous = c;
    }
    return std::max(maxWidth, width);
}
void Canvas::begin(float width, float height, std::vector<Vertex>& output) {
    width_ = std::max(1.f, width);
    height_ = std::max(1.f, height);
    output_ = &output;
    output.clear();
    clips_.clear();
    hits_.clear();
}
void Canvas::clip(Rect rect) {
    clips_.push_back(clips_.empty() ? intersection(rect, {0, 0, width_, height_})
                                    : intersection(rect, clips_.back()));
}
void Canvas::resetClip() {
    clips_.clear();
}
void Canvas::popClip() {
    if (!clips_.empty())
        clips_.pop_back();
}
void Canvas::triangle(Point a, Point b, Point c, Color ca, Color cb, Color cc) {
    if (!output_)
        return;
    std::vector<PaintPoint> polygon = {{{a.x, a.y}, ca}, {{b.x, b.y}, cb}, {{c.x, c.y}, cc}};
    if (!clips_.empty()) {
        Rect clip = clips_.back();
        if (clip.w <= 0 || clip.h <= 0)
            return;
        for (int side = 0; side < 4; ++side) {
            auto distance = [&](Point p) {
                switch (side) {
                case 0:
                    return p.x - clip.x;
                case 1:
                    return clip.x + clip.w - p.x;
                case 2:
                    return p.y - clip.y;
                default:
                    return clip.y + clip.h - p.y;
                }
            };
            std::vector<PaintPoint> next;
            if (polygon.empty())
                return;
            auto previous = polygon.back();
            float prevD = distance(previous.p);
            for (auto current : polygon) {
                float currentD = distance(current.p);
                if ((currentD >= 0) != (prevD >= 0))
                    next.push_back(interpolate(previous, current, prevD / (prevD - currentD)));
                if (currentD >= 0)
                    next.push_back(current);
                previous = current;
                prevD = currentD;
            }
            polygon = std::move(next);
        }
    }
    auto add = [&](PaintPoint p) {
        output_->push_back(
            {p.p.x * 2 / width_ - 1, 1 - p.p.y * 2 / height_, 0, 0, p.c.r, p.c.g, p.c.b, p.c.a, 0});
    };
    for (size_t i = 1; i + 1 < polygon.size(); ++i) {
        add(polygon[0]);
        add(polygon[i]);
        add(polygon[i + 1]);
    }
}
void Canvas::rect(Rect r, Color c) {
    gradient(r, c, c, c, c);
}
void Canvas::gradient(Rect r, Color tl, Color tr, Color br, Color bl) {
    if (r.w <= 0 || r.h <= 0)
        return;
    triangle({r.x, r.y}, {r.x + r.w, r.y}, {r.x + r.w, r.y + r.h}, tl, tr, br);
    triangle({r.x, r.y}, {r.x + r.w, r.y + r.h}, {r.x, r.y + r.h}, tl, br, bl);
}
void Canvas::line(float x1, float y1, float x2, float y2, float width, Color c) {
    float dx = x2 - x1, dy = y2 - y1, length = std::sqrt(dx * dx + dy * dy);
    if (length < .0001f || width <= 0)
        return;
    float nx = -dy / length * width * .5f, ny = dx / length * width * .5f;
    triangle({x1 + nx, y1 + ny}, {x2 + nx, y2 + ny}, {x2 - nx, y2 - ny}, c, c, c);
    triangle({x1 + nx, y1 + ny}, {x2 - nx, y2 - ny}, {x1 - nx, y1 - ny}, c, c, c);
}
void Canvas::circle(float cx, float cy, float radius, Color c, int parts) {
    parts = std::max(3, parts);
    for (int i = 0; i < parts; ++i) {
        float a = float(i) * 2 * pi / parts, b = float(i + 1) * 2 * pi / parts;
        triangle({cx, cy}, {cx + std::cos(a) * radius, cy + std::sin(a) * radius},
                 {cx + std::cos(b) * radius, cy + std::sin(b) * radius}, c, c, c);
    }
}
void Canvas::arc(float cx, float cy, float radius, float from, float to, float width, Color c,
                 int parts) {
    parts = std::max(1, parts);
    Point previous{cx + std::cos(from) * radius, cy + std::sin(from) * radius};
    for (int i = 1; i <= parts; ++i) {
        float a = from + (to - from) * i / parts;
        Point next{cx + std::cos(a) * radius, cy + std::sin(a) * radius};
        line(previous.x, previous.y, next.x, next.y, width, c);
        previous = next;
    }
}
void Canvas::roundedRect(Rect r, float radius, Color c) {
    radius = std::clamp(radius, 0.f, std::min(r.w, r.h) * .5f);
    rect({r.x + radius, r.y, r.w - 2 * radius, r.h}, c);
    rect({r.x, r.y + radius, radius, r.h - 2 * radius}, c);
    rect({r.x + r.w - radius, r.y + radius, radius, r.h - 2 * radius}, c);
    for (int corner = 0; corner < 4; ++corner) {
        float cx = corner == 0 || corner == 3 ? r.x + radius : r.x + r.w - radius,
              cy = corner < 2 ? r.y + radius : r.y + r.h - radius;
        float start = (-1.f + corner * .5f) * pi;
        for (int i = 0; i < 12; ++i) {
            float a = start + i * pi / 24, b = start + (i + 1) * pi / 24;
            triangle({cx, cy}, {cx + std::cos(a) * radius, cy + std::sin(a) * radius},
                     {cx + std::cos(b) * radius, cy + std::sin(b) * radius}, c, c, c);
        }
    }
}
void Canvas::path(const std::vector<Point>& points, float width, Color color, bool closed) {
    for (size_t i = 1; i < points.size(); ++i)
        line(points[i - 1].x, points[i - 1].y, points[i].x, points[i].y, width, color);
    if (closed && points.size() > 1)
        line(points.back().x, points.back().y, points.front().x, points.front().y, width, color);
}
void Canvas::texturedRect(Rect destination, Rect uv, Color color, float mode) {
    if (!output_ || destination.w <= 0 || destination.h <= 0)
        return;
    Rect visible = clips_.empty() ? destination : intersection(destination, clips_.back());
    if (visible.w <= 0 || visible.h <= 0)
        return;
    float u0 = uv.x + (visible.x - destination.x) / destination.w * uv.w,
          v0 = uv.y + (visible.y - destination.y) / destination.h * uv.h,
          u1 = u0 + visible.w / destination.w * uv.w, v1 = v0 + visible.h / destination.h * uv.h;
    auto add = [&](float x, float y, float u, float v) {
        output_->push_back({x * 2 / width_ - 1, 1 - y * 2 / height_, u, v, color.r, color.g,
                            color.b, color.a, mode});
    };
    add(visible.x, visible.y, u0, v0);
    add(visible.x + visible.w, visible.y, u1, v0);
    add(visible.x + visible.w, visible.y + visible.h, u1, v1);
    add(visible.x, visible.y, u0, v0);
    add(visible.x + visible.w, visible.y + visible.h, u1, v1);
    add(visible.x, visible.y + visible.h, u0, v1);
}
void Canvas::image(Rect destination, Rect uv, Color tint) {
    texturedRect(destination, uv, tint, 2);
}
void Canvas::text(float x, float y, std::string_view str, const TextStyle& style) {
    if (!ready)
        return;
    auto& f = face(style);
    float scale = style.size / f.rasterSize, pen = x, baseline = y + fontMetrics(style).ascent;
    uint32_t previous = 0;
    size_t count = 0;
    for (size_t at = 0; at < str.size();) {
        uint32_t c = decode(str, at);
        if (c == '\n') {
            pen = x;
            baseline += style.size * style.lineHeight;
            previous = 0;
            count = 0;
            continue;
        }
        if (count++)
            pen += style.tracking;
        if (previous)
            pen += stbtt_GetCodepointKernAdvance(&f.info, int(previous), int(c)) *
                   stbtt_ScaleForMappingEmToPixels(&f.info, style.size);
        auto& g = f.glyphs[index(c)];
        texturedRect({pen + g.xoff * scale, baseline + g.yoff * scale, (g.x1 - g.x0) * scale,
                      (g.y1 - g.y0) * scale},
                     {float(g.x0) / atlasWidth, float(g.y0) / atlasHeight,
                      float(g.x1 - g.x0) / atlasWidth, float(g.y1 - g.y0) / atlasHeight},
                     style.color, 1);
        pen += g.xadvance * scale;
        previous = c;
    }
}
void Canvas::textCentered(float x, float y, std::string_view str, const TextStyle& style) {
    text(x - measureText(str, style) * .5f, y, str, style);
}
void Canvas::textRight(float x, float y, std::string_view str, const TextStyle& style) {
    text(x - measureText(str, style), y, str, style);
}
float Canvas::wrappedText(Rect area, std::string_view str, const TextStyle& style) {
    clip(area);
    float y = area.y;
    size_t lineStart = 0, lineEnd = 0, at = 0;
    float step = style.size * style.lineHeight;
    while (at < str.size()) {
        size_t wordStart = at;
        while (at < str.size() && str[at] != ' ' && str[at] != '\n')
            ++at;
        size_t wordEnd = at;
        bool newLine = at < str.size() && str[at] == '\n';
        if (lineEnd > lineStart &&
            measureText(str.substr(lineStart, wordEnd - lineStart), style) > area.w) {
            text(area.x, y, str.substr(lineStart, lineEnd - lineStart), style);
            y += step;
            lineStart = wordStart;
        }
        lineEnd = wordEnd;
        if (newLine) {
            text(area.x, y, str.substr(lineStart, lineEnd - lineStart), style);
            y += step;
            lineStart = at + 1;
            lineEnd = lineStart;
        }
        if (at < str.size())
            ++at;
    }
    if (lineEnd > lineStart) {
        text(area.x, y, str.substr(lineStart, lineEnd - lineStart), style);
        y += step;
    }
    popClip();
    return y - area.y;
}
void Canvas::hitbox(uint32_t id, Rect rect) {
    if (!clips_.empty())
        rect = intersection(rect, clips_.back());
    if (rect.w > 0 && rect.h > 0)
        hits_.push_back({id, rect});
}
uint32_t Canvas::hitTest(float x, float y) const {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it)
        if (it->rect.contains(x, y))
            return it->id;
    return 0;
}

Point Layout::measure() const {
    float main = 0, cross = 0;
    bool row = style.axis == Axis::Row;
    for (auto& child : children) {
        Point s = child.measure();
        main += row ? s.x : s.y;
        cross = std::max(cross, row ? s.y : s.x);
    }
    if (!children.empty())
        main += style.gap * (children.size() - 1);
    Point size = row ? Point{main, cross} : Point{cross, main};
    size.x += style.padding.left + style.padding.right;
    size.y += style.padding.top + style.padding.bottom;
    return {std::max({size.x, style.minimum.x, style.preferred.x}),
            std::max({size.y, style.minimum.y, style.preferred.y})};
}
void Layout::arrange(Rect area) {
    bounds = area;
    bool row = style.axis == Axis::Row;
    float available = row ? area.w - style.padding.left - style.padding.right
                          : area.h - style.padding.top - style.padding.bottom;
    float needed = style.gap * std::max(0, int(children.size()) - 1), grow = 0;
    for (auto& child : children) {
        Point s = child.measure();
        needed += row ? s.x : s.y;
        grow += child.style.grow;
    }
    float cursor = row ? area.x + style.padding.left : area.y + style.padding.top;
    float crossAvailable = row ? area.h - style.padding.top - style.padding.bottom
                               : area.w - style.padding.left - style.padding.right;
    for (auto& child : children) {
        Point measured = child.measure();
        float main = row ? measured.x : measured.y;
        if (grow > 0)
            main += std::max(0.f, available - needed) * child.style.grow / grow;
        float cross = row ? measured.y : measured.x;
        float offset = 0;
        if (style.cross == Align::Stretch)
            cross = crossAvailable;
        else if (style.cross == Align::Center)
            offset = (crossAvailable - cross) * .5f;
        else if (style.cross == Align::End)
            offset = crossAvailable - cross;
        child.arrange(row ? Rect{cursor, area.y + style.padding.top + offset, main, cross}
                          : Rect{area.x + style.padding.left + offset, cursor, cross, main});
        cursor += main + style.gap;
    }
}
} // namespace engine::ui
