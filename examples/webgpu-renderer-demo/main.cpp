#include "engine/platform/web/gpu_surface.hpp"
#include "engine/platform/web/host.hpp"
#include "engine/renderer.hpp"
#include "workload.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <emscripten.h>
#include <emscripten/eventloop.h>
#include <engine/physics/physics_world.hpp>
#include <engine/runtime/fixed_step_clock.hpp>
#include <engine/runtime/requested_exchange.hpp>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {
constexpr int MaxObjects = 100000;
constexpr int MaxVisibleDraws = 4096;
constexpr int PbrDrawCount = 35;
constexpr float FixedStep = 1.0f / 120.0f;
struct DrawBatch {
    std::vector<engine::Draw> draws;
    engine::demo::Mode mode = engine::demo::Mode::Renderer;
    engine::demo::WorkloadMetrics metrics{};
    uint64_t config = 0, extractionCount = 0;
    uint32_t bufferGrowths = 0;
    double publishedAt = 0;
    float cameraRadius = 24, cameraTargetX = 0, cameraTargetY = 0, cameraTargetZ = 0;
    DrawBatch() {
        draws.reserve(MaxVisibleDraws);
    }
};
void publishUiMetrics(const DrawBatch& batch);
engine::Renderer renderer;
engine::runtime::RequestedExchange<DrawBatch> exchange;
std::unique_ptr<engine::jobs::Executor> executor;
std::unique_ptr<engine::demo::Workload> workload;
engine::demo::Mode activeMode = engine::demo::Mode::Renderer;
uint64_t activeGeneration = 0;
int activeCount = 100;
std::atomic<int> requestedCount{100}, requestedMode{0};
std::atomic<uint64_t> configGeneration{1}, simulationTicks{0}, extractions{0}, allocationGrowths{0};
std::atomic<float> cameraAngle{0.55f};
std::atomic<int> reportedDraws{0}, packetAgeMs{0}, reportedMode{0};
std::atomic<uint64_t> reportedTicks{0}, discardedClockMicros{0}, reportedExtractions{0};
std::atomic<uint32_t> reportedBufferGrowths{0};
std::atomic<uint32_t> reportedVersion{0};
std::atomic<uint64_t> reportedEntities{0}, reportedSystems{0}, reportedTasks{0}, reportedHelpers{0},
    reportedChecksum{0}, reportedConfig{0};
std::atomic<uint32_t> simulationUs{0}, renderUs{0}, reportedCalls{0}, reportedStepUs{0},
    reportedExtractionUs{0};
std::atomic<unsigned> reportedPeakTasks{0}, helperPoolSize{0}, requestedHelperPoolSize{0};
std::atomic<bool> rendererReady{false}, rendererFailed{false}, workloadFailed{false},
    sceneReady{false};
std::atomic<bool> forceWorkloadReset{false};
std::vector<int> materialIds;
std::array<int, 4> cubeMeshes{}, sphereMeshes{};
std::array<int, PbrDrawCount> pbrMeshes{};
engine::runtime::RequestedExchange<DrawBatch>::ReadHandle currentPacket;
double previousFrame = 0;
uint64_t lastPacketConfig = 0;
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
    const engine::V3 pbrColor{.82f, .42f, .22f};
    for (int row = 0; row < 5; ++row) {
        for (int column = 0; column < 7; ++column) {
            engine::Material material;
            material.color = pbrColor;
            material.rough = .04f + .96f * float(column) / 6.0f;
            material.metal = float(row) / 4.0f;
            materialIds.push_back(renderer.addMaterial(material));
            // The material is assigned by the mesh's material index at creation.
            pbrMeshes[size_t(row * 7 + column)] =
                renderer.addMesh(sphereGeometry(), materialIds.back(), {}, .62f);
        }
    }
    sceneReady.store(true, std::memory_order_release);
}
engine::demo::MeshIds meshIds() {
    return {cubeMeshes, sphereMeshes};
}
engine::demo::Mode checkedMode(int mode) {
    return static_cast<engine::demo::Mode>(std::clamp(mode, 0, 4));
}
std::unique_ptr<engine::demo::Workload> createWorkload(engine::demo::Mode mode, int count) {
    if (mode == engine::demo::Mode::Ecs)
        return engine::demo::createEcsWorkload(*executor, meshIds(), size_t(count));
    if (mode == engine::demo::Mode::Jobs)
        return engine::demo::createJobsWorkload(*executor, meshIds(), size_t(count));
    if (mode == engine::demo::Mode::Physics)
        return engine::demo::createPhysicsWorkload(*executor, meshIds(), size_t(count));
    return {};
}
bool applyPendingConfiguration(engine::demo::Mode& activeMode, uint64_t& activeGeneration,
                               int& activeCount) {
    const uint64_t generation = configGeneration.load(std::memory_order_acquire);
    if (generation == activeGeneration || !sceneReady.load(std::memory_order_acquire))
        return false;
    const bool reset = forceWorkloadReset.exchange(false, std::memory_order_acq_rel);
    const auto mode = checkedMode(requestedMode.load(std::memory_order_relaxed));
    const int count = std::max(1, requestedCount.load(std::memory_order_relaxed));
    if (mode == engine::demo::Mode::Renderer || mode == engine::demo::Mode::Pbr) {
        workload.reset();
    } else if (!reset && mode == activeMode && workload) {
        if (!workload->configure(size_t(count))) {
            workloadFailed.store(true, std::memory_order_release);
            return false;
        }
    } else {
        auto next = createWorkload(mode, count);
        if (!next) {
            workloadFailed.store(true, std::memory_order_release);
            return false;
        }
        workload = std::move(next);
    }
    activeMode = mode;
    activeGeneration = generation;
    activeCount = count;
    simulationTicks.store(0, std::memory_order_relaxed);
    extractions.store(0, std::memory_order_relaxed);
    workloadFailed.store(false, std::memory_order_release);
    return true;
}
void fillBatch(DrawBatch& batch, engine::demo::Mode mode, int count, uint64_t ticks) {
    const size_t before = batch.draws.capacity();
    batch.draws.clear();
    if (mode == engine::demo::Mode::Pbr) {
        for (int row = 0; row < 5; ++row)
            for (int column = 0; column < 7; ++column) {
                const float x = float(column - 3) * 2.15f;
                const float z = float(row - 2) * 2.15f;
                batch.draws.push_back(
                    {pbrMeshes[size_t(row * 7 + column)],
                     engine::translate({x, 1.4f, z}) * engine::scale({1.35f, 1.35f, 1.35f})});
            }
        batch.mode = mode;
        batch.metrics = {};
        batch.metrics.entities = PbrDrawCount;
        batch.metrics.visibleDraws = PbrDrawCount;
        batch.metrics.ticks = ticks;
        batch.cameraRadius = 15.f;
        batch.cameraTargetX = batch.cameraTargetZ = 0;
        batch.cameraTargetY = 1.4f;
        batch.config = activeGeneration;
        batch.publishedAt = engine::platform::web::seconds();
        return;
    }
    const int visible = std::clamp(count, 1, MaxVisibleDraws);
    const int columns = int(std::ceil(std::sqrt(double(visible))));
    const float spacing = 1.8f;
    for (int i = 0; i < visible; ++i) {
        const int x = i % columns, z = i / columns;
        const float phase = float(i) * .173f;
        const float px = (float(x) - float(columns - 1) * .5f) * spacing;
        const float pz = (float(z) - float(columns - 1) * .5f) * spacing;
        const float y = .08f * std::sin(float(ticks) * .015f + phase);
        batch.draws.push_back({(i % 3 == 0) ? sphereMeshes[i % 4] : cubeMeshes[i % 4],
                               engine::translate({px, y, pz}) * engine::rotateY(phase) *
                                   engine::scale({.78f, .78f, .78f})});
    }
    if (batch.draws.capacity() != before)
        allocationGrowths.fetch_add(1, std::memory_order_relaxed);
    batch.mode = mode;
    batch.metrics = {};
    batch.metrics.entities = size_t(count);
    batch.metrics.visibleDraws = batch.draws.size();
    batch.metrics.ticks = ticks;
    batch.cameraRadius = std::max(24.f, float(columns) * .9f * 1.65f);
    batch.cameraTargetX = batch.cameraTargetZ = 0;
    batch.cameraTargetY = 0;
    batch.config = activeGeneration;
}
void produceBatch(DrawBatch& batch) {
    if (rendererFailed.load(std::memory_order_acquire) ||
        workloadFailed.load(std::memory_order_acquire))
        return;
    const double extractionStarted = engine::platform::web::seconds();
    if (activeMode == engine::demo::Mode::Renderer || activeMode == engine::demo::Mode::Pbr) {
        const uint64_t ticks = simulationTicks.load(std::memory_order_relaxed);
        fillBatch(batch, activeMode, activeCount, ticks);
    } else {
        if (!workload)
            return;
        const size_t beforeCapacity = batch.draws.capacity();
        workload->extract(batch.draws);
        if (batch.draws.capacity() != beforeCapacity)
            allocationGrowths.fetch_add(1, std::memory_order_relaxed);
        if (batch.draws.size() > MaxVisibleDraws)
            batch.draws.resize(MaxVisibleDraws);
        batch.mode = activeMode;
        batch.metrics = workload->metrics();
        batch.metrics.extractionMilliseconds =
            (engine::platform::web::seconds() - extractionStarted) * 1000.0;
        batch.metrics.visibleDraws = batch.draws.size();
        float minX = 0, maxX = 0, minZ = 0, maxZ = 0;
        const size_t firstVisible = activeMode == engine::demo::Mode::Physics ? 1 : 0;
        if (batch.draws.size() > firstVisible) {
            minX = maxX = batch.draws[firstVisible].transform.m[12];
            minZ = maxZ = batch.draws[firstVisible].transform.m[14];
            for (size_t i = firstVisible + 1; i < batch.draws.size(); ++i) {
                minX = std::min(minX, batch.draws[i].transform.m[12]);
                maxX = std::max(maxX, batch.draws[i].transform.m[12]);
                minZ = std::min(minZ, batch.draws[i].transform.m[14]);
                maxZ = std::max(maxZ, batch.draws[i].transform.m[14]);
            }
        }
        batch.cameraTargetX = (minX + maxX) * .5f;
        batch.cameraTargetZ = (minZ + maxZ) * .5f;
        const float extent = std::max(maxX - minX, maxZ - minZ);
        batch.cameraRadius =
            activeMode == engine::demo::Mode::Physics ? 30.f : std::max(24.f, extent * .72f + 12.f);
        batch.cameraTargetY = activeMode == engine::demo::Mode::Physics ? 2.0f : 0.0f;
        batch.config = activeGeneration;
    }
    batch.metrics.extractionMilliseconds =
        (engine::platform::web::seconds() - extractionStarted) * 1000.0;
    batch.publishedAt = engine::platform::web::seconds();
    batch.extractionCount = extractions.fetch_add(1, std::memory_order_relaxed) + 1;
    batch.bufferGrowths = uint32_t(allocationGrowths.load(std::memory_order_relaxed));
}
void simulationPump() {
    static engine::runtime::FixedStepClock clock;
    static double previousPump = engine::platform::web::seconds();
    const double started = engine::platform::web::seconds();
    const double now = engine::platform::web::seconds();
    clock.beginFrame(now - previousPump);
    previousPump = now;
    discardedClockMicros.store(uint64_t(clock.discardedSeconds() * 1000000.0),
                               std::memory_order_relaxed);
    if (applyPendingConfiguration(activeMode, activeGeneration, activeCount)) {
        clock.reset();
        previousPump = now;
        discardedClockMicros.store(0, std::memory_order_relaxed);
        simulationTicks.store(0, std::memory_order_relaxed);
        extractions.store(0, std::memory_order_relaxed);
        allocationGrowths.store(0, std::memory_order_relaxed);
    }
    while (clock.consumeStep()) {
        if (activeMode == engine::demo::Mode::Renderer || !workload) {
            simulationTicks.fetch_add(1, std::memory_order_relaxed);
        } else if (workload->step(FixedStep)) {
            simulationTicks.fetch_add(1, std::memory_order_relaxed);
        } else {
            workloadFailed.store(true, std::memory_order_release);
            break;
        }
    }
    if (sceneReady.load(std::memory_order_acquire) && exchange.requested())
        exchange.produce(produceBatch);
    simulationUs.store(uint32_t((engine::platform::web::seconds() - started) * 1000000),
                       std::memory_order_relaxed);
}
void renderFrame() {
    if (!rendererReady.load(std::memory_order_acquire))
        return;
    const double now = engine::platform::web::seconds();
    double cadenceMs = previousFrame > 0 ? (now - previousFrame) * 1000 : 0;
    auto packet = exchange.acquire();
    if (packet) {
        currentPacket = std::move(packet);
        if (currentPacket->config != lastPacketConfig) {
            lastPacketConfig = currentPacket->config;
            cadenceSamples.fill(0);
            renderSamples.fill(0);
            sampleCount = sampleCursor = 0;
            framesSincePercentiles = 0;
            cadenceP50.store(0, std::memory_order_relaxed);
            cadenceP95.store(0, std::memory_order_relaxed);
            renderP50.store(0, std::memory_order_relaxed);
            renderP95.store(0, std::memory_order_relaxed);
            previousFrame = 0;
            cadenceMs = 0;
        }
    }
    if (!currentPacket)
        return;
    exchange.request();
    publishUiMetrics(*currentPacket);
    packetAgeMs.store(int(std::max(0.0, (now - currentPacket->publishedAt) * 1000)),
                      std::memory_order_relaxed);
    const double started = engine::platform::web::seconds();
    const float angle = cameraAngle.load(std::memory_order_relaxed);
    const float radius = currentPacket->cameraRadius;
    const engine::V3 target{currentPacket->cameraTargetX, currentPacket->cameraTargetY,
                            currentPacket->cameraTargetZ};
    const engine::V3 eye{target.x + std::sin(angle) * radius, target.y + radius * .68f,
                         target.z + std::cos(angle) * radius};
    const float lightingTime = currentPacket->mode == engine::demo::Mode::Pbr ? .9f : float(now);
    renderer.render(currentPacket->draws, eye, target, target, lightingTime, .9f);
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
struct UiMetrics {
    int mode = 0, draws = 0;
    uint64_t ticks = 0, config = 0, entities = 0, systems = 0, tasks = 0, helpers = 0, checksum = 0,
             extractions = 0;
    uint32_t bufferGrowths = 0;
    uint32_t stepUs = 0, extractionUs = 0;
    unsigned peakTasks = 0;
};
void publishUiMetrics(const DrawBatch& batch) {
    reportedVersion.fetch_add(1, std::memory_order_acq_rel);
    reportedDraws.store(int(batch.draws.size()), std::memory_order_relaxed);
    reportedMode.store(int(batch.mode), std::memory_order_relaxed);
    reportedTicks.store(batch.metrics.ticks, std::memory_order_relaxed);
    reportedConfig.store(batch.config, std::memory_order_relaxed);
    reportedEntities.store(batch.metrics.entities, std::memory_order_relaxed);
    reportedSystems.store(batch.metrics.systems, std::memory_order_relaxed);
    reportedTasks.store(batch.metrics.tasks, std::memory_order_relaxed);
    reportedHelpers.store(batch.metrics.helperCompletedDelta, std::memory_order_relaxed);
    reportedPeakTasks.store(batch.metrics.peakConcurrentTasks, std::memory_order_relaxed);
    reportedChecksum.store(batch.metrics.checksum, std::memory_order_relaxed);
    reportedStepUs.store(uint32_t(std::max(0.0, batch.metrics.stepMilliseconds) * 1000),
                         std::memory_order_relaxed);
    reportedExtractionUs.store(uint32_t(std::max(0.0, batch.metrics.extractionMilliseconds) * 1000),
                               std::memory_order_relaxed);
    reportedBufferGrowths.store(batch.bufferGrowths, std::memory_order_relaxed);
    reportedExtractions.store(batch.extractionCount, std::memory_order_relaxed);
    reportedVersion.fetch_add(1, std::memory_order_release);
}
UiMetrics readUiMetrics() {
    UiMetrics value;
    static UiMetrics lastComplete;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t before = reportedVersion.load(std::memory_order_acquire);
        if (before & 1U)
            continue;
        value.draws = reportedDraws.load(std::memory_order_relaxed);
        value.mode = reportedMode.load(std::memory_order_relaxed);
        value.ticks = reportedTicks.load(std::memory_order_relaxed);
        value.config = reportedConfig.load(std::memory_order_relaxed);
        value.entities = reportedEntities.load(std::memory_order_relaxed);
        value.systems = reportedSystems.load(std::memory_order_relaxed);
        value.tasks = reportedTasks.load(std::memory_order_relaxed);
        value.helpers = reportedHelpers.load(std::memory_order_relaxed);
        value.peakTasks = reportedPeakTasks.load(std::memory_order_relaxed);
        value.checksum = reportedChecksum.load(std::memory_order_relaxed);
        value.stepUs = reportedStepUs.load(std::memory_order_relaxed);
        value.extractionUs = reportedExtractionUs.load(std::memory_order_relaxed);
        value.extractions = reportedExtractions.load(std::memory_order_relaxed);
        value.bufferGrowths = reportedBufferGrowths.load(std::memory_order_relaxed);
        if (reportedVersion.load(std::memory_order_acquire) == before) {
            lastComplete = value;
            return lastComplete;
        }
    }
    return lastComplete;
}
void report(void*) {
    const UiMetrics m = readUiMetrics();
    std::ostringstream data;
    data << "{\"cadenceP50\":" << cadenceP50.load(std::memory_order_relaxed)
         << ",\"cadenceP95\":" << cadenceP95.load(std::memory_order_relaxed)
         << ",\"renderUs\":" << renderUs.load(std::memory_order_relaxed)
         << ",\"simulationUs\":" << simulationUs.load(std::memory_order_relaxed)
         << ",\"ticks\":" << m.ticks << ",\"draws\":" << m.draws
         << ",\"packetAge\":" << packetAgeMs.load(std::memory_order_relaxed)
         << ",\"extractions\":" << m.extractions
         << ",\"renderP50\":" << renderP50.load(std::memory_order_relaxed)
         << ",\"renderP95\":" << renderP95.load(std::memory_order_relaxed)
         << ",\"growths\":" << m.bufferGrowths
         << ",\"frames\":" << renderedFrames.load(std::memory_order_relaxed)
         << ",\"mode\":" << m.mode << ",\"calls\":" << reportedCalls.load(std::memory_order_relaxed)
         << ",\"error\":"
         << ((rendererFailed.load(std::memory_order_relaxed) ||
              workloadFailed.load(std::memory_order_relaxed))
                 ? "true"
                 : "false")
         << ",\"entities\":" << m.entities << ",\"systems\":" << m.systems
         << ",\"tasks\":" << m.tasks << ",\"helpers\":" << m.helpers << ",\"config\":" << m.config
         << ",\"requestedConfig\":" << configGeneration.load(std::memory_order_relaxed)
         << ",\"peakTasks\":" << m.peakTasks << ",\"checksum\":\"" << m.checksum
         << "\",\"stepUs\":" << m.stepUs << ",\"extractionUs\":" << m.extractionUs
         << ",\"activeHelpers\":" << helperPoolSize.load(std::memory_order_relaxed)
         << ",\"requestedHelpers\":" << requestedHelperPoolSize.load(std::memory_order_relaxed)
         << ",\"discardedUs\":" << discardedClockMicros.load(std::memory_order_relaxed) << '}';
    const std::string json = data.str();
    EM_ASM({
        const d = JSON.parse(UTF8ToString($0));
        const put = (id, value) => { const node = document.getElementById(id); if (node) node.textContent = value; };
        const modeName = d.mode === 0 ? "Renderer" : d.mode === 1 ? "ECS hierarchy" : d.mode === 2 ? "Job systems" : d.mode === 3 ? "Jolt physics" : d.mode === 4 ? "PBR material grid" : "Starting";
        put("metric-fps", d.cadenceP50 > 0 ? (1000000 / d.cadenceP50).toFixed(1) : "—");
        put("metric-cadence", d.cadenceP50 > 0 ? "p50 " + (d.cadenceP50 / 1000).toFixed(2) + " ms · p95 " + (d.cadenceP95 / 1000).toFixed(2) + " ms" : "—");
        put("metric-cpu", (d.renderUs / 1000).toFixed(2) + " ms · p50 " + (d.renderP50 / 1000).toFixed(2) + " · p95 " + (d.renderP95 / 1000).toFixed(2));
        put("metric-sim", (d.simulationUs / 1000).toFixed(3) + " ms");
        put("metric-ticks", String(d.ticks));
        put("metric-draws", String(d.draws));
        put("metric-age", d.packetAge + " ms");
        put("metric-extractions", String(d.extractions));
        put("metric-grows", String(d.growths));
        put("metric-frames", String(d.frames));
        put("metric-calls", String(d.calls));
        put("metric-mode", modeName + (d.config === d.requestedConfig ? "" : " · applying selection"));
        put("metric-entities", String(d.entities));
        put("metric-work", d.systems + " systems · " + d.tasks + " tasks");
        put("metric-helpers", d.helpers + " completed · peak " + d.peakTasks);
        put("metric-checksum", d.checksum);
        put("metric-step", (d.stepUs / 1000).toFixed(3) + " ms · extract " + (d.extractionUs / 1000).toFixed(3) + " ms");
        put("metric-pool", d.activeHelpers + " active · " + d.requestedHelpers + " requested");
        put("metric-discarded", (d.discardedUs / 1000000).toFixed(3) + " s");
        const error = document.getElementById("render-error");
        error.hidden = !d.error;
        if (d.error) error.textContent = "The renderer or workload reported an error. Check browser console and selected configuration.";
    }, json.c_str());
}
unsigned requestedHelpers() {
    const unsigned hardware = engine::platform::web::hardwareThreadCount();
    const unsigned cap = hardware > 2 ? hardware - 2 : 0;
    const int choice = EM_ASM_INT({
        const value = new URLSearchParams(location.search).get('helpers');
        if (!value || value === 'auto') return -1;
        const parsed = Number(value);
        return Number.isInteger(parsed) && parsed >= 0 ? parsed : -2;
    });
    const unsigned desired = choice < 0 ? cap : unsigned(choice);
    return std::min(cap, desired);
}
} // namespace

extern "C" {
EMSCRIPTEN_KEEPALIVE void set_object_count(int count) {
    if (requestedMode.load(std::memory_order_relaxed) != int(engine::demo::Mode::Pbr))
        requestedCount.store(std::clamp(count, 1, MaxObjects), std::memory_order_relaxed);
    configGeneration.fetch_add(1, std::memory_order_release);
    exchange.request();
}
EMSCRIPTEN_KEEPALIVE void set_demo_mode(int mode) {
    requestedMode.store(std::clamp(mode, 0, 4), std::memory_order_relaxed);
    configGeneration.fetch_add(1, std::memory_order_release);
    exchange.request();
}
EMSCRIPTEN_KEEPALIVE void set_camera_angle(float angle) {
    cameraAngle.store(angle, std::memory_order_relaxed);
}
EMSCRIPTEN_KEEPALIVE void reset_demo() {
    forceWorkloadReset.store(true, std::memory_order_release);
    configGeneration.fetch_add(1, std::memory_order_release);
    exchange.request();
}
}
int main() {
    engine::platform::web::installInput({});
    engine::physics::initialize_jolt();
    const unsigned requested = requestedHelpers();
    requestedHelperPoolSize.store(requested, std::memory_order_relaxed);
    executor = std::make_unique<engine::jobs::Executor>(requested);
    helperPoolSize.store(executor->workerCount(), std::memory_order_relaxed);
    if (!engine::platform::web::startWorkers(renderStart, simulationStart))
        rendererFailed.store(true, std::memory_order_relaxed);
    emscripten_set_interval(report, 250, nullptr);
}
