#pragma once
#include <webgpu/webgpu_cpp.h>
namespace engine::platform::web {
wgpu::Surface createSurface(wgpu::Instance instance, const char* selector);
}
