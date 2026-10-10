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
void installInput(const std::vector<int>& handledKeys);
std::vector<Event> drainEvents();
void run(void (*frame)());
double loadNumber(const char* key);
void saveNumber(const char* key, double value);
void openLink(const char* url);
void reload();
} // namespace engine::platform::web
