#pragma once
#include <string>
#include <vector>

namespace engine::platform::web {
struct Viewport {
    float width = 1, height = 1, scale = 1;
    bool coarsePointer = false;
};
enum class EventType { KeyDown, KeyUp, PointerDown, PointerUp, PointerMove, Wheel, FocusLost };
struct Event {
    EventType type;
    int code = 0, id = 0;
    float x = 0, y = 0, delta = 0;
    bool repeat = false;
    double seconds = 0;
    bool shift = false;
};
double seconds();
Viewport viewport();
void resizeCanvas(int width, int height);
// The observer runs synchronously on the browser main thread before the event is
// queued for render-side UI consumption.
void installInput(const std::vector<int>& handledKeys, void (*observer)(const Event&) = nullptr);
// Reuses the caller's vector capacity. Queue overflow discards stale input and
// reports FocusLost so consumers can clear held state.
void drainEvents(std::vector<Event>& events);
// Launches the render and simulation owners without waiting for their startup
// callbacks to finish. The render owner receives the #game OffscreenCanvas.
bool startWorkers(void (*renderStart)(), void (*simulationStart)());
// These enter an Emscripten event loop on the calling owner and do not return.
void runRender(void (*frame)());
void runSimulation(void (*pump)(), int intervalMs = 2);
bool isMainThread();
// Reports the browser's positive hardwareConcurrency value, or 8 when absent.
unsigned hardwareThreadCount();
// Browser compute helpers after reserving render and simulation owners.
unsigned computeWorkerCount();
// Worker calls are asynchronous. userData must remain valid until callback runs.
void dispatchMain(void (*callback)(void*), void* userData);
void run(void (*frame)());
double loadNumber(const char* key);
void saveNumber(const char* key, double value);
void openLink(const char* url);
void reload();
} // namespace engine::platform::web
