#include "engine/platform/web/gpu_surface.hpp"
#include "engine/platform/web/host.hpp"
#include "engine/renderer.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <emscripten.h>
#include <emscripten/eventloop.h>
#include <engine/runtime/fixed_step_clock.hpp>
#include <engine/runtime/requested_exchange.hpp>
#include <vector>

namespace {
constexpr int MaxObjects = 4000;
struct DrawBatch {
    std::vector<engine::Draw> draws;
    double publishedAt = 0;
    DrawBatch() {
        draws.reserve(MaxObjects);
    }
};
engine::Renderer renderer;
engine::runtime::RequestedExchange<DrawBatch> exchange;
std::atomic<int> requestedCount{100};
std::atomic<float> cameraAngle{0.55f};
std::atomic<uint64_t> simulationTicks{0}, extractions{0}, allocationGrowths{0};
std::atomic<int> reportedDraws{0}, packetAgeMs{0};
std::atomic<uint32_t> simulationUs{0}, renderUs{0}, reportedCalls{0};
std::atomic<bool> rendererReady{false}, rendererFailed{false};
std::vector<int> materialIds;
std::array<int, 4> cubeMeshes{}, sphereMeshes{};
std::atomic<bool> sceneReady{false};
engine::runtime::RequestedExchange<DrawBatch>::ReadHandle currentPacket;
double previousFrame = 0;
std::array<uint32_t, 120> cadenceSamples{}, renderSamples{};
size_t sampleCount = 0, sampleCursor = 0;
unsigned framesSincePercentiles = 0;
std::atomic<uint32_t> cadenceP50{0}, cadenceP95{0}, renderP50{0}, renderP95{0};
std::atomic<uint64_t> renderedFrames{0};

engine::Geometry cubeGeometry() {
    engine::Geometry g;
    const float r = .5f;
    g.quad({-r, -r, r}, {r, -r, r}, {r, r, r}, {-r, r, r}, {1, 1, 1});
    g.quad({r, -r, -r}, {-r, -r, -r}, {-r, r, -r}, {r, r, -r}, {1, 1, 1});
    g.quad({-r, -r, -r}, {-r, -r, r}, {-r, r, r}, {-r, r, -r}, {1, 1, 1});
    g.quad({r, -r, r}, {r, -r, -r}, {r, r, -r}, {r, r, r}, {1, 1, 1});
    g.quad({-r, r, r}, {r, r, r}, {r, r, -r}, {-r, r, -r}, {1, 1, 1});
    g.quad({-r, -r, -r}, {r, -r, -r}, {r, -r, r}, {-r, -r, r}, {1, 1, 1});
    return g;
}
engine::Geometry sphereGeometry() {
    engine::Geometry g;
    constexpr int rings = 12, slices = 20;
    for (int y = 0; y <= rings; ++y) {
        const float v = float(y) / rings;
        const float phi = v * engine::pi;
        for (int x = 0; x <= slices; ++x) {
            const float u = float(x) / slices;
            const float theta = u * 2 * engine::pi;
            engine::V3 n{std::sin(phi) * std::cos(theta), std::cos(phi),
                         std::sin(phi) * std::sin(theta)};
            g.v.push_back({n * .5f, n, u, v, {1, 1, 1}, 0});
        }
    }
    for (int y = 0; y < rings; ++y)
        for (int x = 0; x < slices; ++x) {
            const uint32_t a = uint32_t(y * (slices + 1) + x), b = a + slices + 1;
            g.i.insert(g.i.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    return g;
}
void makeScene() {
    const engine::V3 colors[] = {
        {.92f, .24f, .12f}, {.16f, .48f, .91f}, {.18f, .72f, .48f}, {.92f, .68f, .16f}};
    for (int i = 0; i < 4; ++i) {
        engine::Material material;
        material.color = colors[i];
        material.rough = .18f + .2f * float(i);
        material.metal = .12f + .24f * float(i % 3);
        materialIds.push_back(renderer.addMaterial(material));
    }
    for (int i = 0; i < 4; ++i) {
        cubeMeshes[i] = renderer.addMesh(cubeGeometry(), materialIds[i], {}, .9f);
        sphereMeshes[i] = renderer.addMesh(sphereGeometry(), materialIds[i], {}, .9f);
    }
}
void fillBatch(DrawBatch& batch) {
    const size_t before = batch.draws.capacity();
    batch.draws.clear();
    const int count = std::clamp(requestedCount.load(std::memory_order_relaxed), 1, MaxObjects);
    const int columns = int(std::ceil(std::sqrt(double(count))));
    const float spacing = 1.8f;
    for (int i = 0; i < count; ++i) {
        const int x = i % columns, z = i / columns;
        const float phase = float(i) * .173f;
        const float px = (float(x) - float(columns - 1) * .5f) * spacing;
        const float pz = (float(z) - float(columns - 1) * .5f) * spacing;
        const float y =
            .08f * std::sin(float(simulationTicks.load(std::memory_order_relaxed)) * .015f + phase);
        batch.draws.push_back({(i % 3 == 0) ? sphereMeshes[i % 4] : cubeMeshes[i % 4],
                               engine::translate({px, y, pz}) * engine::rotateY(phase) *
                                   engine::scale({.78f, .78f, .78f})});
    }
    if (batch.draws.capacity() != before)
        allocationGrowths.fetch_add(1, std::memory_order_relaxed);
    batch.publishedAt = engine::platform::web::seconds();
    reportedDraws.store(int(batch.draws.size()), std::memory_order_relaxed);
    extractions.fetch_add(1, std::memory_order_relaxed);
}
void simulationPump() {
    static engine::runtime::FixedStepClock clock;
    static double previousPump = engine::platform::web::seconds();
    const double started = engine::platform::web::seconds();
    clock.beginFrame(started - previousPump);
    previousPump = started;
    while (clock.consumeStep())
        simulationTicks.fetch_add(1, std::memory_order_relaxed);
    if (sceneReady.load(std::memory_order_acquire) && exchange.requested())
        exchange.produce(fillBatch);
    simulationUs.store(uint32_t((engine::platform::web::seconds() - started) * 1000000),
                       std::memory_order_relaxed);
}
void renderFrame() {
    if (!rendererReady.load(std::memory_order_acquire))
        return;
    const double now = engine::platform::web::seconds();
    const double cadenceMs = previousFrame > 0 ? (now - previousFrame) * 1000 : 0;
    auto packet = exchange.acquire();
    if (packet)
        currentPacket = std::move(packet);
    if (!currentPacket)
        return;
    exchange.request();
    packetAgeMs.store(int(std::max(0.0, (now - currentPacket->publishedAt) * 1000)),
                      std::memory_order_relaxed);
    const double started = engine::platform::web::seconds();
    const float angle = cameraAngle.load(std::memory_order_relaxed);
    const int count = int(currentPacket->draws.size());
    const int columns = int(std::ceil(std::sqrt(double(std::max(1, count)))));
    const float extent = float(columns) * .9f;
    const float radius = std::max(24.f, extent * 1.65f);
    const engine::V3 target{0, 0, 0};
    const engine::V3 eye{std::sin(angle) * radius, radius * .68f, std::cos(angle) * radius};
    renderer.render(currentPacket->draws, eye, target, target, float(now), .9f);
    const double renderMs = (engine::platform::web::seconds() - started) * 1000;
    renderUs.store(uint32_t(renderMs * 1000), std::memory_order_relaxed);
    reportedCalls.store(uint32_t(renderer.drawCalls), std::memory_order_relaxed);
    renderedFrames.fetch_add(1, std::memory_order_relaxed);
    if (cadenceMs > 0) {
        cadenceSamples[sampleCursor] = uint32_t(cadenceMs * 1000);
        renderSamples[sampleCursor] = uint32_t(renderMs * 1000);
        sampleCursor = (sampleCursor + 1) % cadenceSamples.size();
        sampleCount = std::min(sampleCount + 1, cadenceSamples.size());
        if (++framesSincePercentiles == 30) {
            auto cadence = cadenceSamples;
            auto render = renderSamples;
            std::sort(cadence.begin(), cadence.begin() + sampleCount);
            std::sort(render.begin(), render.begin() + sampleCount);
            const auto p50 = sampleCount / 2;
            const auto p95 = std::min(sampleCount - 1, (sampleCount * 95) / 100);
            cadenceP50.store(cadence[p50], std::memory_order_relaxed);
            cadenceP95.store(cadence[p95], std::memory_order_relaxed);
            renderP50.store(render[p50], std::memory_order_relaxed);
            renderP95.store(render[p95], std::memory_order_relaxed);
            framesSincePercentiles = 0;
        }
    }
    previousFrame = now;
}
void rendererStarted(bool ready) {
    rendererFailed.store(!ready, std::memory_order_release);
    if (!ready)
        return;
    makeScene();
    renderer.resize(1024, 640);
    sceneReady.store(true, std::memory_order_release);
    exchange.request();
    rendererReady.store(true, std::memory_order_release);
}
void renderStart() {
    engine::Renderer::HostCallbacks callbacks;
    callbacks.createSurface = [](wgpu::Instance instance) {
        return engine::platform::web::createSurface(instance, "#game");
    };
    callbacks.reportError = [](const char*) {
        rendererFailed.store(true, std::memory_order_release);
    };
    renderer.init(rendererStarted, std::move(callbacks));
    engine::platform::web::runRender(renderFrame);
}
void simulationStart() {
    engine::platform::web::runSimulation(simulationPump, 2);
}
void report(void*) {
    EM_ASM({
        const put = (id, value) => { const node = document.getElementById(id); if (node) node.textContent = value; };
        put('metric-fps', $0 > 0 ? (1000000 / $0).toFixed(1) : '—');
        put('metric-cadence', $0 > 0 ? 'p50 ' + ($0 / 1000).toFixed(2) + ' ms · p95 ' + ($1 / 1000).toFixed(2) + ' ms' : '—');
        put('metric-cpu', ($2 / 1000).toFixed(2) + ' ms · p50 ' + ($8 / 1000).toFixed(2) + ' · p95 ' + ($9 / 1000).toFixed(2));
        put('metric-sim', ($3 / 1000).toFixed(3) + ' ms');
        put('metric-ticks', String($4));
        put('metric-draws', String($5));
        put('metric-age', $6 + ' ms');
        put('metric-extractions', String($7));
        put('metric-grows', String($10));
        put('metric-frames', String($11));
        put('metric-calls', String($13));
        const error = document.getElementById('render-error');
        if ($12) error.hidden = false;
    },
           cadenceP50.load(std::memory_order_relaxed),
           cadenceP95.load(std::memory_order_relaxed),
           renderUs.load(std::memory_order_relaxed),
           simulationUs.load(std::memory_order_relaxed),
           double(simulationTicks.load(std::memory_order_relaxed)),
           reportedDraws.load(std::memory_order_relaxed),
           packetAgeMs.load(std::memory_order_relaxed),
           double(extractions.load(std::memory_order_relaxed)),
           renderP50.load(std::memory_order_relaxed),
           renderP95.load(std::memory_order_relaxed),
           double(allocationGrowths.load(std::memory_order_relaxed)),
           double(renderedFrames.load(std::memory_order_relaxed)),
           rendererFailed.load(std::memory_order_relaxed),
           reportedCalls.load(std::memory_order_relaxed));
}
} // namespace

extern "C" {
EMSCRIPTEN_KEEPALIVE void set_object_count(int count) {
    requestedCount.store(std::clamp(count, 1, MaxObjects), std::memory_order_relaxed);
    exchange.request();
}
EMSCRIPTEN_KEEPALIVE void set_camera_angle(float angle) {
    cameraAngle.store(angle, std::memory_order_relaxed);
}
}
int main() {
    engine::platform::web::installInput({});
    if (!engine::platform::web::startWorkers(renderStart, simulationStart))
        rendererFailed.store(true, std::memory_order_relaxed);
    emscripten_set_interval(report, 250, nullptr);
}
