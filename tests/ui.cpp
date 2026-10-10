#include "engine/ui/canvas.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace engine::ui;
int main() {
    FontAtlas atlas;
    assert(buildFontAtlas("tests/assets/fonts", atlas));
    assert(fontsReady());
    TextStyle sans{FontFace::Sans, 16};
    TextStyle mono{FontFace::Mono, 16};
    TextStyle large{FontFace::Serif, 112};
    assert(measureText("WWW", sans) > measureText("iii", sans) * 2);
    assert(std::abs(measureText("WWW", mono) - measureText("iii", mono)) < .01f);
    assert(measureText("Corniche", large) > measureText("Corniche", sans) * 4);
    assert(measureText("Côte d’Or", sans) > 0);
    assert(hasGlyph(FontFace::Sans, U'′'));
    assert(!hasGlyph(FontFace::Sans, U'◀')); // Custom icons use geometry, not missing glyphs.
    std::vector<Vertex> output;
    Canvas canvas;
    canvas.begin(100, 100, output);
    canvas.clip({25, 25, 50, 50});
    canvas.rect({0, 0, 100, 100}, {1, 0, 0, .5f});
    for (auto vertex : output) {
        assert(vertex.x >= -.5001f && vertex.x <= .5001f);
        assert(vertex.y >= -.5001f && vertex.y <= .5001f);
    }
    canvas.hitbox(7, {0, 0, 100, 100});
    assert(canvas.hitTest(50, 50) == 7);
    assert(canvas.hitTest(10, 10) == 0);
    canvas.resetClip();
    canvas.text(0, 0, "AV", sans);
    assert(output.size() > 12);
    canvas.begin(100, 100, output);
    canvas.clip({10, 10, 8, 8});
    canvas.text(0, 0, "W", TextStyle{FontFace::Sans, 40});
    for (auto vertex : output) {
        assert(vertex.x >= -.8001f && vertex.x <= -.6399f);
        assert(vertex.y <= .8001f && vertex.y >= .6399f);
        assert(vertex.mode == 1);
        assert(vertex.u >= 0 && vertex.u <= 1 && vertex.v >= 0 && vertex.v <= 1);
    }
    canvas.begin(100, 100, output);
    canvas.clip({25, 0, 50, 100});
    canvas.gradient({0, 0, 100, 100}, {0, 0, 0, 0}, {1, 0, 0, 1}, {1, 0, 0, 1}, {0, 0, 0, 0});
    for (auto vertex : output) {
        assert(vertex.r >= .2499f && vertex.r <= .7501f);
        assert(std::abs(vertex.r - vertex.a) < .0001f);
    }
    Layout row;
    row.style.axis = Axis::Row;
    row.style.gap = 10;
    row.style.cross = Align::Center;
    row.children.resize(2);
    row.children[0].style.preferred = {30, 20};
    row.children[1].style.preferred = {40, 30};
    assert(row.measure().x == 80);
    row.arrange({5, 10, 100, 40});
    assert(row.children[0].bounds.x == 5);
    assert(row.children[1].bounds.x == 45);
    assert(row.children[0].bounds.y == 20);
    std::printf("PASS UI: proportional/mono/UTF8 font metrics, clipping, shared hit boxes, layout; "
                "atlas %dx%d\n",
                atlas.width, atlas.height);
}
