#pragma once
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace engine::graphics {
using ResourceId = std::uint32_t;
using PassId = std::uint32_t;
enum class Kind { Texture, Buffer };
enum class Usage : std::uint32_t {
    Sampled = 1,
    ColorAttachment = 2,
    DepthAttachment = 4,
    Storage = 8,
    CopySource = 16,
    CopyTarget = 32,
    ResolveSource = 64,
    ResolveTarget = 128,
    Present = 256
};
constexpr std::uint32_t mask(Usage u) {
    return std::uint32_t(u);
}
constexpr std::uint32_t operator|(Usage a, Usage b) {
    return mask(a) | mask(b);
}
struct ResourceDesc {
    Kind kind = Kind::Texture;
    std::string format;
    std::uint32_t width = 1, height = 1, samples = 1, usage = 0;
    std::uint64_t bytes = 0;
    bool operator==(const ResourceDesc&) const = default;
};
struct Version {
    ResourceId resource;
    std::uint32_t version;
    bool operator==(const Version&) const = default;
};
struct Access {
    Version version;
    Usage usage;
    bool writing;
    bool preserved = true;
    // An attachment read declares LoadOp::Load of that prior version. A write
    // preserves contents by default; use false for StoreOp::Discard. Resolve
    // can read the just-written multisample version inside the same pass.
    static Access read(Version v, Usage u) {
        return {v, u, false, true};
    }
    static Access write(Version v, Usage u, bool preserved = true) {
        return {v, u, true, preserved};
    }
};
struct Lifetime {
    ResourceId resource;
    std::uint32_t first, last, slot;
    bool imported;
};
struct CompiledPass {
    PassId id;
    std::vector<PassId> prerequisites;
};
// WebGPU owns synchronization. Slots represent compatible whole objects;
// no native heap aliasing or explicit Vulkan-style barriers are implied.
class FrameGraph {
  public:
    static constexpr std::uint32_t noSlot = std::numeric_limits<std::uint32_t>::max();
    ResourceId resource(std::string name, ResourceDesc desc, bool imported = false);
    Version initial(ResourceId id) const {
        return {id, 0};
    }
    Version next(ResourceId id);
    PassId pass(std::string name, std::vector<Access> accesses, std::function<void()> work,
                bool sideEffect = true);
    bool compile();
    bool execute();
    const std::string& error() const {
        return error_;
    }
    const std::vector<CompiledPass>& schedule() const {
        return compiled_;
    }
    const std::vector<Lifetime>& lifetimes() const {
        return lifetimes_;
    }
    const std::vector<ResourceDesc>& slots() const {
        return slots_;
    }

  private:
    struct Resource {
        std::string name;
        ResourceDesc desc;
        bool imported;
        std::uint32_t versions = 0;
    };
    struct Pass {
        std::string name;
        std::vector<Access> accesses;
        std::function<void()> work;
        bool sideEffect;
    };
    std::vector<Resource> resources_;
    std::vector<Pass> passes_;
    std::vector<CompiledPass> compiled_;
    std::vector<Lifetime> lifetimes_;
    std::vector<ResourceDesc> slots_;
    std::string error_;
    bool dirty_ = true;
};
} // namespace engine::graphics
