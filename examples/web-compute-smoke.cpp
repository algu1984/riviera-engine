#include <array>
#include <cstdio>
#include <emscripten.h>
#include <engine/compute_context.hpp>
#include <string>

namespace {
constexpr uint32_t kValueCount = 256;
constexpr uint64_t kByteCount = sizeof(uint32_t) * kValueCount;
engine::ComputeContext context;
wgpu::Buffer inputA, inputB, output, readback;
wgpu::ComputePipeline pipeline;
wgpu::BindGroup bindGroup;
bool computeStarted = false;
bool finished = false;
wgpu::Instance lateCallbackPump;
bool lateInitCallbackCalled = false;
bool computeResultReady = false;
bool computeResultSuccess = false;
std::string computeResultMessage;
uint32_t ticksAfterContextDestroy = 0;

std::string toString(wgpu::StringView text) {
    if (!text.data)
        return "unknown error";
    if (text.length == wgpu::kStrlen)
        return text.data;
    return {text.data, text.length};
}

void finish(bool success, const char* message) {
    if (finished)
        return;
    finished = true;
    std::printf("WEBGPU_COMPUTE_SMOKE_%s: %s\n", success ? "PASS" : "FAIL", message);
    EM_ASM({
        document.body.dataset.computeSmoke = UTF8ToString($0);
        document.body.dataset.computeSmokeMessage = UTF8ToString($1);
    }, success ? "pass" : "fail", message);
}

wgpu::Buffer makeBuffer(wgpu::BufferUsage usage) {
    wgpu::BufferDescriptor descriptor{};
    descriptor.size = kByteCount;
    descriptor.usage = usage;
    return context.Device().CreateBuffer(&descriptor);
}

void mapReadback() {
    readback.MapAsync(wgpu::MapMode::Read, 0, kByteCount, wgpu::CallbackMode::AllowProcessEvents,
                      [](wgpu::MapAsyncStatus status, wgpu::StringView) {
                          if (status != wgpu::MapAsyncStatus::Success) {
                              finish(false, "readback buffer mapping failed");
                              return;
                          }
                          std::array<uint32_t, kValueCount> actual{};
                          if (!readback.ReadMappedRange(0, actual.data(), kByteCount)) {
                              finish(false, "readback data could not be copied");
                              readback.Unmap();
                              return;
                          }
                          for (uint32_t i = 0; i < kValueCount; ++i) {
                              if (actual[i] != (i + 3) + (i * 7 + 11)) {
                                  finish(false, "GPU result differs from CPU reference");
                                  readback.Unmap();
                                  return;
                              }
                          }
                          readback.Unmap();
                          computeResultReady = true;
                          computeResultSuccess = true;
                          computeResultMessage = "256 GPU additions match the CPU reference";
                      });
}

void popErrorScopes() {
    context.Device().PopErrorScope(
        wgpu::CallbackMode::AllowProcessEvents,
        [](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type, wgpu::StringView text) {
            if (status != wgpu::PopErrorScopeStatus::Success) {
                finish(false, "validation error scope could not be read");
                return;
            }
            if (type != wgpu::ErrorType::NoError) {
                std::string detail = toString(text);
                finish(false, ("WebGPU validation error: " + detail).c_str());
                return;
            }
            context.Device().PopErrorScope(
                wgpu::CallbackMode::AllowProcessEvents,
                [](wgpu::PopErrorScopeStatus oomStatus, wgpu::ErrorType oomType,
                   wgpu::StringView oomText) {
                    if (oomStatus != wgpu::PopErrorScopeStatus::Success) {
                        finish(false, "out-of-memory error scope could not be read");
                        return;
                    }
                    if (oomType != wgpu::ErrorType::NoError) {
                        std::string detail = toString(oomText);
                        finish(false, ("WebGPU out-of-memory error: " + detail).c_str());
                        return;
                    }
                    mapReadback();
                });
        });
}

void dispatch() {
    auto device = context.Device();
    device.PushErrorScope(wgpu::ErrorFilter::OutOfMemory);
    device.PushErrorScope(wgpu::ErrorFilter::Validation);
    std::array<uint32_t, kValueCount> a{}, b{};
    for (uint32_t i = 0; i < kValueCount; ++i) {
        a[i] = i + 3;
        b[i] = i * 7 + 11;
    }
    inputA = makeBuffer(wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
    inputB = makeBuffer(wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
    output = makeBuffer(wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
    readback = makeBuffer(wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst);
    context.Queue().WriteBuffer(inputA, 0, a.data(), kByteCount);
    context.Queue().WriteBuffer(inputB, 0, b.data(), kByteCount);

    constexpr char shader[] = R"(
    @group(0) @binding(0) var<storage, read> a: array<u32>;
    @group(0) @binding(1) var<storage, read> b: array<u32>;
    @group(0) @binding(2) var<storage, read_write> result: array<u32>;

    @compute @workgroup_size(64)
    fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
        if (gid.x < 256u) {
            result[gid.x] = a[gid.x] + b[gid.x];
        }
    }
    )";
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = shader;
    wgpu::ShaderModuleDescriptor shaderDescriptor{};
    shaderDescriptor.nextInChain = &wgsl;
    auto module = device.CreateShaderModule(&shaderDescriptor);
    wgpu::ComputeState compute{};
    compute.module = module;
    compute.entryPoint = "main";
    wgpu::ComputePipelineDescriptor pipelineDescriptor{};
    pipelineDescriptor.compute = compute;
    pipeline = device.CreateComputePipeline(&pipelineDescriptor);

    auto layout = pipeline.GetBindGroupLayout(0);
    wgpu::BindGroupEntry entries[3]{};
    entries[0].binding = 0;
    entries[0].buffer = inputA;
    entries[0].size = kByteCount;
    entries[1].binding = 1;
    entries[1].buffer = inputB;
    entries[1].size = kByteCount;
    entries[2].binding = 2;
    entries[2].buffer = output;
    entries[2].size = kByteCount;
    wgpu::BindGroupDescriptor bindGroupDescriptor{};
    bindGroupDescriptor.layout = layout;
    bindGroupDescriptor.entryCount = 3;
    bindGroupDescriptor.entries = entries;
    bindGroup = device.CreateBindGroup(&bindGroupDescriptor);

    auto encoder = device.CreateCommandEncoder();
    auto pass = encoder.BeginComputePass();
    pass.SetPipeline(pipeline);
    pass.SetBindGroup(0, bindGroup);
    pass.DispatchWorkgroups(kValueCount / 64);
    pass.End();
    encoder.CopyBufferToBuffer(output, 0, readback, 0, kByteCount);
    auto commands = encoder.Finish();
    context.Queue().Submit(1, &commands);
    popErrorScopes();
}

void tick() {
    if (lateCallbackPump) {
        lateCallbackPump.ProcessEvents();
        ++ticksAfterContextDestroy;
        if (lateInitCallbackCalled) {
            finish(false, "destroyed compute context received a late init callback");
            return;
        }
    }
    context.ProcessEvents();
    if (computeResultReady && ticksAfterContextDestroy >= 120) {
        finish(computeResultSuccess, computeResultMessage.c_str());
        return;
    }
    if (!computeStarted && context.Ready()) {
        computeStarted = true;
        dispatch();
    } else if (!computeStarted && !context.Error().empty()) {
        computeStarted = true;
        finish(false, context.Error().c_str());
    }
}
} // namespace

int main() {
    {
        engine::ComputeContext shortLived;
        shortLived.Initialize([](bool) { lateInitCallbackCalled = true; });
        lateCallbackPump = shortLived.Instance();
    }
    context.Initialize([](bool success) {
        if (!success) {
            computeStarted = true;
            finish(false, context.Error().c_str());
        }
    });
    emscripten_set_main_loop(tick, 0, false);
}
