#include "engine/platform/web/host.hpp"
#include <algorithm>
#include <emscripten.h>
#include <emscripten/html5.h>

namespace engine::platform::web {
namespace {
std::vector<Event> events;
std::vector<int> handledKeys;
void append(Event e) {
    e.seconds = seconds();
    events.push_back(e);
}
EM_BOOL keyboard(int type, const EmscriptenKeyboardEvent* e, void*) {
    int k = e->keyCode;
    Event event{type == EMSCRIPTEN_EVENT_KEYDOWN ? EventType::KeyDown : EventType::KeyUp,
                k,
                0,
                0,
                0,
                0,
                bool(e->repeat)};
    event.shift = e->shiftKey;
    append(event);
    return std::find(handledKeys.begin(), handledKeys.end(), k) != handledKeys.end();
}
EM_BOOL mouse(int type, const EmscriptenMouseEvent* e, void*) {
    append(
        {type == EMSCRIPTEN_EVENT_MOUSEDOWN
             ? EventType::PointerDown
             : (type == EMSCRIPTEN_EVENT_MOUSEUP ? EventType::PointerUp : EventType::PointerMove),
         0, 0, float(e->targetX), float(e->targetY)});
    return true;
}
EM_BOOL touch(int type, const EmscriptenTouchEvent* e, void*) {
    for (int i = 0; i < e->numTouches; i++) {
        auto& t = e->touches[i];
        if (!t.isChanged)
            continue;
        append({type == EMSCRIPTEN_EVENT_TOUCHSTART
                    ? EventType::PointerDown
                    : (type == EMSCRIPTEN_EVENT_TOUCHMOVE ? EventType::PointerMove
                                                          : EventType::PointerUp),
                0, int(t.identifier) + 1, float(t.targetX), float(t.targetY)});
    }
    return true;
}
EM_BOOL wheel(int, const EmscriptenWheelEvent* e, void*) {
    append({EventType::Wheel, 0, 0, float(e->mouse.targetX), float(e->mouse.targetY),
            float(e->deltaY) * (e->deltaMode == 1 ? 20.f : 1.f)});
    return true;
}
EM_BOOL blur(int, const EmscriptenFocusEvent*, void*) {
    append({EventType::FocusLost});
    return false;
}
EM_BOOL visibility(int, const EmscriptenVisibilityChangeEvent* e, void*) {
    if (e->hidden)
        append({EventType::FocusLost});
    return false;
}
} // namespace
double seconds() {
    return emscripten_get_now() / 1000.;
}
Viewport viewport() {
    double w = 1, h = 1;
    emscripten_get_element_css_size("#game", &w, &h);
    return {float(w), float(h), float(std::min(1.75, emscripten_get_device_pixel_ratio())),
            bool(EM_ASM_INT({ return matchMedia('(pointer: coarse)').matches; }))};
}
void resizeCanvas(int w, int h) {
    emscripten_set_canvas_element_size("#game", w, h);
}
void installInput(const std::vector<int>& keys) {
    handledKeys = keys;
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, keyboard);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, keyboard);
    emscripten_set_mousedown_callback("#game", nullptr, true, mouse);
    emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, mouse);
    emscripten_set_mousemove_callback("#game", nullptr, true, mouse);
    emscripten_set_touchstart_callback("#game", nullptr, true, touch);
    emscripten_set_touchmove_callback("#game", nullptr, true, touch);
    emscripten_set_touchend_callback("#game", nullptr, true, touch);
    emscripten_set_touchcancel_callback("#game", nullptr, true, touch);
    emscripten_set_wheel_callback("#game", nullptr, true, wheel);
    emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, blur);
    emscripten_set_visibilitychange_callback(nullptr, true, visibility);
}
std::vector<Event> drainEvents() {
    std::vector<Event> pending;
    pending.swap(events);
    return pending;
}
void run(void (*frame)()) {
    emscripten_set_main_loop(frame, 0, false);
}
double loadNumber(const char* key) {
    return EM_ASM_DOUBLE(
        {
            try {
                return Number(localStorage.getItem(UTF8ToString($0))) || 0;
            } catch {
                return 0;
            }
        },
        key);
}
void saveNumber(const char* key, double value) {
    EM_ASM(
        {
            try {
                localStorage.setItem(UTF8ToString($0), String($1));
            } catch {
            }
        },
        key, value);
}
void openLink(const char* url) {
    EM_ASM({ window.open(UTF8ToString($0), '_blank', 'noopener'); }, url);
}
void reload() {
    EM_ASM({ location.reload(); });
}
} // namespace engine::platform::web
