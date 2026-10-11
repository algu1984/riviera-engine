#include "engine/platform/web/host.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>
#include <emscripten/threading_legacy.h>
#include <memory>
#include <pthread.h>
#include <string>
#include <utility>

namespace engine::platform::web {
namespace {
constexpr uint32_t EventCapacity = 512;
constexpr size_t WorkerStackSize = 2 * 1024 * 1024;
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<float>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
std::array<Event, EventCapacity> eventQueue{};
std::atomic<uint32_t> eventHead{0}, eventTail{0};
std::atomic<bool> eventOverflow{false};
std::vector<int> handledKeys;
void (*inputObserver)(const Event&) = nullptr;

std::atomic<uint32_t> viewportVersion{0};
std::atomic<float> viewportWidth{1}, viewportHeight{1}, viewportScale{1};
std::atomic<bool> viewportCoarsePointer{false};

void (*renderStartCallback)() = nullptr;
void (*simulationStartCallback)() = nullptr;
bool workerStartupAttempted = false;
bool mainLoopStarted = false;
void (*legacyFrameCallback)() = nullptr;

void append(Event event) {
    event.seconds = seconds();
    if (inputObserver)
        inputObserver(event);
    uint32_t head = eventHead.load(std::memory_order_relaxed);
    uint32_t tail = eventTail.load(std::memory_order_acquire);
    if (head - tail >= EventCapacity) {
        eventOverflow.store(true, std::memory_order_release);
        if (inputObserver && event.type != EventType::FocusLost) {
            Event lost{};
            lost.type = EventType::FocusLost;
            lost.seconds = event.seconds;
            inputObserver(lost);
        }
        return;
    }
    eventQueue[head % EventCapacity] = event;
    eventHead.store(head + 1, std::memory_order_release);
}

EM_BOOL keyboard(int type, const EmscriptenKeyboardEvent* event, void*) {
    int code = event->keyCode;
    Event input{};
    input.type = type == EMSCRIPTEN_EVENT_KEYDOWN ? EventType::KeyDown : EventType::KeyUp;
    input.code = code;
    input.repeat = event->repeat;
    input.shift = event->shiftKey;
    append(input);
    return std::find(handledKeys.begin(), handledKeys.end(), code) != handledKeys.end();
}

EM_BOOL mouse(int type, const EmscriptenMouseEvent* event, void*) {
    Event input{};
    input.type =
        type == EMSCRIPTEN_EVENT_MOUSEDOWN
            ? EventType::PointerDown
            : (type == EMSCRIPTEN_EVENT_MOUSEUP ? EventType::PointerUp : EventType::PointerMove);
    input.x = float(event->targetX);
    input.y = float(event->targetY);
    append(input);
    return true;
}

EM_BOOL touch(int type, const EmscriptenTouchEvent* event, void*) {
    for (int i = 0; i < event->numTouches; ++i) {
        const auto& touch = event->touches[i];
        if (!touch.isChanged)
            continue;
        Event input{};
        input.type = type == EMSCRIPTEN_EVENT_TOUCHSTART
                         ? EventType::PointerDown
                         : (type == EMSCRIPTEN_EVENT_TOUCHMOVE ? EventType::PointerMove
                                                               : EventType::PointerUp);
        input.id = int(touch.identifier) + 1;
        input.x = float(touch.targetX);
        input.y = float(touch.targetY);
        append(input);
    }
    return true;
}

EM_BOOL wheel(int, const EmscriptenWheelEvent* event, void*) {
    Event input{};
    input.type = EventType::Wheel;
    input.x = float(event->mouse.targetX);
    input.y = float(event->mouse.targetY);
    input.delta = float(event->deltaY) * (event->deltaMode == DOM_DELTA_LINE ? 20.f : 1.f);
    append(input);
    return true;
}

void appendFocusLost() {
    Event event{};
    event.type = EventType::FocusLost;
    append(event);
}

EM_BOOL blur(int, const EmscriptenFocusEvent*, void*) {
    appendFocusLost();
    return false;
}

EM_BOOL visibility(int, const EmscriptenVisibilityChangeEvent* event, void*) {
    if (event->hidden)
        appendFocusLost();
    return false;
}

void publishViewport() {
    double width = 1, height = 1;
    emscripten_get_element_css_size("#game", &width, &height);
    float scale = float(std::min(1.75, emscripten_get_device_pixel_ratio()));
    bool coarse = EM_ASM_INT({ return matchMedia('(pointer: coarse)').matches; });
    viewportVersion.fetch_add(1, std::memory_order_seq_cst);
    viewportWidth.store(float(width), std::memory_order_seq_cst);
    viewportHeight.store(float(height), std::memory_order_seq_cst);
    viewportScale.store(scale, std::memory_order_seq_cst);
    viewportCoarsePointer.store(coarse, std::memory_order_seq_cst);
    viewportVersion.fetch_add(1, std::memory_order_seq_cst);
}

void mainPump() {
    publishViewport();
}

void legacyMainFrame() {
    publishViewport();
    if (legacyFrameCallback)
        legacyFrameCallback();
}

void* renderThread(void*) {
    if (renderStartCallback)
        renderStartCallback();
    emscripten_exit_with_live_runtime();
}

void* simulationThread(void*) {
    if (simulationStartCallback)
        simulationStartCallback();
    emscripten_exit_with_live_runtime();
}

void saveNumberOnMain(void* data) {
    std::unique_ptr<std::pair<std::string, double>> value(
        static_cast<std::pair<std::string, double>*>(data));
    EM_ASM(
        {
            try {
                localStorage.setItem(UTF8ToString($0), String($1));
            } catch {
            }
        },
        value->first.c_str(), value->second);
}

void openLinkOnMain(void* data) {
    std::unique_ptr<std::string> url(static_cast<std::string*>(data));
    EM_ASM({ window.open(UTF8ToString($0), '_blank', 'noopener'); }, url->c_str());
}

void reloadOnMain(void*) {
    EM_ASM({ location.reload(); });
}
} // namespace

double seconds() {
    return emscripten_get_now() / 1000.;
}

Viewport viewport() {
    static thread_local Viewport lastValid{};
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint32_t before = viewportVersion.load(std::memory_order_seq_cst);
        if (before & 1u)
            continue;
        Viewport result;
        result.width = viewportWidth.load(std::memory_order_seq_cst);
        result.height = viewportHeight.load(std::memory_order_seq_cst);
        result.scale = viewportScale.load(std::memory_order_seq_cst);
        result.coarsePointer = viewportCoarsePointer.load(std::memory_order_seq_cst);
        uint32_t after = viewportVersion.load(std::memory_order_seq_cst);
        if (before == after && !(after & 1u)) {
            lastValid = result;
            return result;
        }
    }
    return lastValid;
}

void resizeCanvas(int width, int height) {
    emscripten_set_canvas_element_size("#game", width, height);
}

void installInput(const std::vector<int>& keys, void (*observer)(const Event&)) {
    handledKeys = keys;
    inputObserver = observer;
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

void drainEvents(std::vector<Event>& events) {
    events.clear();
    if (eventOverflow.exchange(false, std::memory_order_acq_rel)) {
        eventTail.store(eventHead.load(std::memory_order_acquire), std::memory_order_release);
        Event lost{};
        lost.type = EventType::FocusLost;
        lost.seconds = seconds();
        events.push_back(lost);
    }
    uint32_t tail = eventTail.load(std::memory_order_relaxed);
    uint32_t head = eventHead.load(std::memory_order_acquire);
    while (tail != head) {
        events.push_back(eventQueue[tail % EventCapacity]);
        ++tail;
        eventTail.store(tail, std::memory_order_release);
    }
}

bool startWorkers(void (*renderStart)(), void (*simulationStart)()) {
    if (workerStartupAttempted || !renderStart || !simulationStart)
        return false;

    workerStartupAttempted = true;
    renderStartCallback = renderStart;
    simulationStartCallback = simulationStart;
    publishViewport();
    if (!mainLoopStarted) {
        emscripten_set_main_loop(mainPump, 20, false);
        mainLoopStarted = true;
    }

    pthread_attr_t renderAttributes;
    pthread_attr_init(&renderAttributes);
    int result = emscripten_pthread_attr_settransferredcanvases(&renderAttributes, "#game");
    if (result == 0)
        result = pthread_attr_setstacksize(&renderAttributes, WorkerStackSize);
    pthread_t renderWorker{};
    if (result == 0)
        result = pthread_create(&renderWorker, &renderAttributes, renderThread, nullptr);
    pthread_attr_destroy(&renderAttributes);
    if (result != 0)
        return false;

    pthread_attr_t simulationAttributes;
    pthread_attr_init(&simulationAttributes);
    result = pthread_attr_setstacksize(&simulationAttributes, WorkerStackSize);
    pthread_t simulationWorker{};
    if (result == 0)
        result =
            pthread_create(&simulationWorker, &simulationAttributes, simulationThread, nullptr);
    pthread_attr_destroy(&simulationAttributes);
    if (result != 0)
        return false;

    return true;
}

void runRender(void (*frame)()) {
    if (frame)
        emscripten_set_main_loop(frame, 0, true);
}

void runSimulation(void (*pump)(), int intervalMs) {
    if (!pump)
        return;
    int interval = std::max(1, intervalMs);
    int rate = std::max(1, int(1000.0 / interval));
    emscripten_set_main_loop(pump, rate, true);
}

bool isMainThread() {
    return emscripten_is_main_runtime_thread();
}

void dispatchMain(void (*callback)(void*), void* userData) {
    if (!callback)
        return;
    if (isMainThread()) {
        callback(userData);
        return;
    }
    emscripten_async_run_in_main_runtime_thread(EM_FUNC_SIG_VI, callback, userData);
}

void run(void (*frame)()) {
    if (!frame)
        return;
    if (mainLoopStarted)
        return;
    legacyFrameCallback = frame;
    mainLoopStarted = true;
    emscripten_set_main_loop(legacyMainFrame, 0, false);
}

double loadNumber(const char* key) {
    if (!isMainThread())
        return 0;
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
    if (isMainThread()) {
        EM_ASM(
            {
                try {
                    localStorage.setItem(UTF8ToString($0), String($1));
                } catch {
                }
            },
            key, value);
        return;
    }
    dispatchMain(saveNumberOnMain, new std::pair<std::string, double>(key, value));
}

void openLink(const char* url) {
    if (isMainThread()) {
        EM_ASM({ window.open(UTF8ToString($0), '_blank', 'noopener'); }, url);
        return;
    }
    dispatchMain(openLinkOnMain, new std::string(url));
}

void reload() {
    dispatchMain(reloadOnMain, nullptr);
}
} // namespace engine::platform::web
