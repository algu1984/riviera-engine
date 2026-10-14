#pragma once

#include <functional>
#include <memory>
#include <string>
#include <webgpu/webgpu_cpp.h>

namespace engine {

// Surface-free WebGPU device ownership for compute and other headless work.
// Call ProcessEvents from the owning event loop when callbacks use
// CallbackMode::AllowProcessEvents. The returned handles remain owned by the
// context and are intended for single-owner use on the caller's GPU thread;
// release copied handles before destroying the context.
// Clients own error scopes for their pipelines, dispatches and readbacks.
class ComputeContext {
  public:
    using InitCallback = std::function<void(bool)>;

    ComputeContext();
    ~ComputeContext();
    ComputeContext(const ComputeContext&) = delete;
    ComputeContext& operator=(const ComputeContext&) = delete;
    ComputeContext(ComputeContext&&) = delete;
    ComputeContext& operator=(ComputeContext&&) = delete;

    void Initialize(InitCallback callback);
    bool Ready() const;
    const std::string& Error() const;
    wgpu::Instance Instance() const;
    wgpu::Adapter Adapter() const;
    wgpu::Device Device() const;
    wgpu::Queue Queue() const;
    void ProcessEvents() const;

  private:
    struct State;
    std::shared_ptr<State> state;
};

} // namespace engine
