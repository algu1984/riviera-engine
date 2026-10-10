#include "engine/platform/web/gpu_surface.hpp"
#include <webgpu/webgpu_cpp.h>
namespace engine::platform::web {
wgpu::Surface createSurface(wgpu::Instance instance, const char* selector) {
    wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector source{};
    source.selector = selector;
    wgpu::SurfaceDescriptor descriptor{};
    descriptor.nextInChain = &source;
    return instance.CreateSurface(&descriptor);
}
} // namespace engine::platform::web
