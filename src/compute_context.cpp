#include "engine/compute_context.hpp"

namespace engine {
struct ComputeContext::State {
    // Keep callback state alive until after device handles are released.
    std::string error;
    bool ready = false;
    bool initialized = false;
    wgpu::Instance instance;
    wgpu::Adapter adapter;
    wgpu::Device device;
    wgpu::Queue queue;
};

namespace {
std::string message(wgpu::StringView view) {
    if (!view.data)
        return {};
    if (view.length == wgpu::kStrlen)
        return view.data;
    return {view.data, view.length};
}
} // namespace

ComputeContext::ComputeContext() : state(std::make_shared<State>()) {}

ComputeContext::~ComputeContext() = default;

void ComputeContext::Initialize(InitCallback callback) {
    auto current = state;
    if (current->initialized) {
        current->error = "Compute context initialization was already requested";
        if (callback)
            callback(false);
        return;
    }
    current->initialized = true;
    current->instance = wgpu::CreateInstance();
    if (!current->instance) {
        current->error = "WebGPU instance creation failed";
        if (callback)
            callback(false);
        return;
    }

    std::weak_ptr<State> weak = current;
    wgpu::RequestAdapterOptions options{};
    options.powerPreference = wgpu::PowerPreference::HighPerformance;
    current->instance.RequestAdapter(
        &options, wgpu::CallbackMode::AllowProcessEvents,
        [weak, callback = std::move(callback)](wgpu::RequestAdapterStatus status,
                                               wgpu::Adapter adapter, wgpu::StringView text) {
            auto context = weak.lock();
            if (!context)
                return;
            if (status != wgpu::RequestAdapterStatus::Success) {
                context->error = message(text);
                if (context->error.empty())
                    context->error = "No WebGPU adapter is available";
                if (callback)
                    callback(false);
                return;
            }
            context->adapter = adapter;
            wgpu::Limits supported{};
            if (!adapter.GetLimits(&supported)) {
                context->error = "WebGPU could not report adapter limits";
                if (callback)
                    callback(false);
                return;
            }
            wgpu::Limits required{};
            required.maxBufferSize = supported.maxBufferSize;
            required.maxStorageBufferBindingSize = supported.maxStorageBufferBindingSize;
            wgpu::DeviceDescriptor descriptor{};
            descriptor.requiredLimits = &required;
            descriptor.SetDeviceLostCallback(
                wgpu::CallbackMode::AllowProcessEvents,
                [weak](const wgpu::Device&, wgpu::DeviceLostReason, wgpu::StringView reason) {
                    if (auto state = weak.lock()) {
                        state->ready = false;
                        state->error = message(reason);
                        if (state->error.empty())
                            state->error = "The compute WebGPU device was lost";
                    }
                });
            context->adapter.RequestDevice(
                &descriptor, wgpu::CallbackMode::AllowProcessEvents,
                [weak, callback](wgpu::RequestDeviceStatus deviceStatus, wgpu::Device device,
                                 wgpu::StringView deviceMessage) {
                    auto state = weak.lock();
                    if (!state)
                        return;
                    if (deviceStatus != wgpu::RequestDeviceStatus::Success) {
                        state->error = message(deviceMessage);
                        if (state->error.empty())
                            state->error = "WebGPU device creation failed";
                        if (callback)
                            callback(false);
                        return;
                    }
                    state->device = device;
                    state->queue = device.GetQueue();
                    state->ready = true;
                    state->error.clear();
                    if (callback)
                        callback(true);
                });
        });
}

bool ComputeContext::Ready() const {
    return state->ready;
}

const std::string& ComputeContext::Error() const {
    return state->error;
}

wgpu::Instance ComputeContext::Instance() const {
    return state->instance;
}

wgpu::Adapter ComputeContext::Adapter() const {
    return state->adapter;
}

wgpu::Device ComputeContext::Device() const {
    return state->device;
}

wgpu::Queue ComputeContext::Queue() const {
    return state->queue;
}

void ComputeContext::ProcessEvents() const {
    if (state->instance)
        state->instance.ProcessEvents();
}
} // namespace engine
