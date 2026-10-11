#include "engine/platform/web/gpu_surface.hpp"
#include "engine/platform/web/host.hpp"
#include <atomic>
#include <emscripten.h>
#include <emscripten/eventloop.h>

namespace {
std::atomic<unsigned> renderFrames{0};
std::atomic<unsigned> simulationPumps{0};
std::atomic<bool> surfaceCreated{false};
std::atomic<bool> surfaceConfigured{false};
std::atomic<bool> surfaceTextureAcquired{false};
std::atomic<bool> renderRanOnWorker{false};
std::atomic<bool> simulationRanOnWorker{false};
wgpu::Surface surface;
wgpu::Instance instance;
wgpu::Adapter adapter;
wgpu::Device device;

void renderFrame() {
    renderRanOnWorker.store(!engine::platform::web::isMainThread(), std::memory_order_relaxed);
    if (surface && device && !surfaceTextureAcquired.load(std::memory_order_relaxed)) {
        wgpu::SurfaceTexture texture{};
        surface.GetCurrentTexture(&texture);
        if (texture.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal ||
            texture.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal)
            surfaceTextureAcquired.store(true, std::memory_order_relaxed);
    }
    renderFrames.fetch_add(1, std::memory_order_relaxed);
}

void simulationPump() {
    simulationRanOnWorker.store(!engine::platform::web::isMainThread(), std::memory_order_relaxed);
    simulationPumps.fetch_add(1, std::memory_order_relaxed);
}

void renderStart() {
    instance = wgpu::CreateInstance();
    surface = engine::platform::web::createSurface(instance, "#game");
    surfaceCreated.store(bool(surface), std::memory_order_release);

    wgpu::RequestAdapterOptions options{};
    instance.RequestAdapter(
        &options, wgpu::CallbackMode::AllowSpontaneous,
        [](wgpu::RequestAdapterStatus status, wgpu::Adapter requestedAdapter, wgpu::StringView) {
            if (status != wgpu::RequestAdapterStatus::Success)
                return;
            adapter = requestedAdapter;
            wgpu::DeviceDescriptor descriptor{};
            adapter.RequestDevice(&descriptor, wgpu::CallbackMode::AllowSpontaneous,
                                  [](wgpu::RequestDeviceStatus deviceStatus,
                                     wgpu::Device requestedDevice, wgpu::StringView) {
                                      if (deviceStatus != wgpu::RequestDeviceStatus::Success)
                                          return;
                                      device = requestedDevice;
                                      wgpu::SurfaceCapabilities capabilities{};
                                      surface.GetCapabilities(adapter, &capabilities);
                                      if (!capabilities.formatCount)
                                          return;
                                      wgpu::SurfaceConfiguration configuration{};
                                      configuration.device = device;
                                      configuration.format = capabilities.formats[0];
                                      configuration.usage = wgpu::TextureUsage::RenderAttachment;
                                      configuration.width = 32;
                                      configuration.height = 32;
                                      configuration.presentMode = wgpu::PresentMode::Fifo;
                                      configuration.alphaMode = wgpu::CompositeAlphaMode::Opaque;
                                      surface.Configure(&configuration);
                                      surfaceConfigured.store(true, std::memory_order_release);
                                      engine::platform::web::runRender(renderFrame);
                                  });
        });
}

void simulationStart() {
    engine::platform::web::runSimulation(simulationPump, 2);
}

void report(void*) {
    EM_ASM({
        const result = document.getElementById('result');
        result.dataset.renderFrames = $0;
        result.dataset.simulationPumps = $1;
        result.dataset.surfaceCreated = $2;
        result.dataset.surfaceConfigured = $3;
        result.dataset.surfaceTextureAcquired = $4;
        result.dataset.renderWorker = $5;
        result.dataset.simulationWorker = $6;
    },
           int(renderFrames.load(std::memory_order_relaxed)),
           int(simulationPumps.load(std::memory_order_relaxed)),
           int(surfaceCreated.load(std::memory_order_relaxed)),
           int(surfaceConfigured.load(std::memory_order_relaxed)),
           int(surfaceTextureAcquired.load(std::memory_order_relaxed)),
           int(renderRanOnWorker.load(std::memory_order_relaxed)),
           int(simulationRanOnWorker.load(std::memory_order_relaxed)));
}
} // namespace

int main() {
    if (!engine::platform::web::startWorkers(renderStart, simulationStart)) {
        EM_ASM({ document.getElementById('result').textContent = 'worker launch failed'; });
        return 1;
    }
    emscripten_set_interval(report, 50, nullptr);
}
