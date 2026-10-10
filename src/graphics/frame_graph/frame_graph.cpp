#include "engine/graphics/frame_graph/frame_graph.hpp"
#include <algorithm>
#include <deque>
#include <map>
#include <set>

namespace engine::graphics {
ResourceId FrameGraph::resource(std::string name, ResourceDesc desc, bool imported) {
    dirty_ = true;
    resources_.push_back({std::move(name), std::move(desc), imported});
    return ResourceId(resources_.size() - 1);
}
Version FrameGraph::next(ResourceId id) {
    dirty_ = true;
    if (id >= resources_.size())
        return {id, 0};
    return {id, ++resources_[id].versions};
}
PassId FrameGraph::pass(std::string name, std::vector<Access> accesses, std::function<void()> work,
                        bool sideEffect) {
    dirty_ = true;
    passes_.push_back({std::move(name), std::move(accesses), std::move(work), sideEffect});
    return PassId(passes_.size() - 1);
}
bool FrameGraph::compile() {
    error_.clear();
    compiled_.clear();
    lifetimes_.clear();
    slots_.clear();
    auto fail = [&](std::string text) {
        error_ = std::move(text);
        return false;
    };
    using Key = std::pair<ResourceId, std::uint32_t>;
    std::map<Key, PassId> writers;
    std::map<Key, bool> preserved;
    std::map<Key, std::vector<PassId>> readers;
    std::vector<std::set<PassId>> dependencies(passes_.size());
    for (PassId p = 0; p < passes_.size(); ++p) {
        const auto& pass = passes_[p];
        std::uint32_t attachmentWidth = 0, attachmentHeight = 0, attachmentSamples = 0;
        std::map<ResourceId, std::uint32_t> usages;
        for (const auto& a : pass.accesses) {
            if (a.version.resource >= resources_.size())
                return fail("Unknown resource in " + pass.name);
            const auto& r = resources_[a.version.resource];
            const auto& d = r.desc;
            if (!d.width || !d.height || !d.samples)
                return fail("Invalid dimensions: " + r.name);
            if (a.version.version > r.versions)
                return fail("Unknown resource version: " + r.name);
            if (!(d.usage & mask(a.usage)))
                return fail("Unsupported resource usage: " + r.name + " in " + pass.name);
            if (a.writing && (a.usage == Usage::Sampled || a.usage == Usage::CopySource ||
                              a.usage == Usage::ResolveSource || a.usage == Usage::Present))
                return fail("Read-only usage declared as write in " + pass.name);
            if (!a.writing && (a.usage == Usage::CopyTarget || a.usage == Usage::ResolveTarget))
                return fail("Write-only usage declared as read in " + pass.name);
            const bool depth = d.format.find("depth") != std::string::npos;
            if (a.usage == Usage::DepthAttachment && (d.kind != Kind::Texture || !depth))
                return fail("Depth attachment format: " + r.name);
            if (a.usage == Usage::ColorAttachment && (d.kind != Kind::Texture || depth))
                return fail("Color attachment format: " + r.name);
            if (a.usage == Usage::ColorAttachment || a.usage == Usage::DepthAttachment ||
                a.usage == Usage::ResolveSource) {
                if (attachmentSamples &&
                    (attachmentSamples != d.samples || attachmentWidth != d.width ||
                     attachmentHeight != d.height))
                    return fail("Attachment dimensions/sample mismatch in " + pass.name);
                attachmentSamples = d.samples;
                attachmentWidth = d.width;
                attachmentHeight = d.height;
            }
            if (a.usage == Usage::ResolveSource && d.samples <= 1)
                return fail("Resolve source must be multisampled: " + r.name);
            if (a.usage == Usage::ResolveTarget && d.samples != 1)
                return fail("Resolve target must have one sample: " + r.name);
            usages[a.version.resource] |= mask(a.usage);
            Key key{a.version.resource, a.version.version};
            if (a.writing) {
                if (!a.version.version)
                    return fail("Cannot overwrite initial resource version: " + r.name);
                if (!writers.emplace(key, p).second)
                    return fail("Ambiguous write to " + r.name);
                preserved[key] = a.preserved;
            } else
                readers[key].push_back(p);
        }
        for (auto [id, use] : usages) {
            (void)id;
            if ((use & mask(Usage::Sampled)) &&
                (use & (mask(Usage::ColorAttachment) | mask(Usage::DepthAttachment) |
                        mask(Usage::ResolveTarget))))
                return fail("Sampled attachment conflict in " + pass.name);
        }
        for (auto& a : pass.accesses)
            if (a.usage == Usage::ResolveTarget) {
                const auto& d = resources_[a.version.resource].desc;
                bool source = false;
                for (auto& b : pass.accesses)
                    if (b.usage == Usage::ResolveSource) {
                        const auto& s = resources_[b.version.resource].desc;
                        source |=
                            s.width == d.width && s.height == d.height && s.format == d.format;
                    }
                if (!source)
                    return fail("Resolve target has no compatible source in " + pass.name);
            }
    }
    for (auto& [key, rs] : readers) {
        auto w = writers.find(key);
        if (w == writers.end()) {
            if (key.second || !resources_[key.first].imported)
                return fail("Read before write: " + resources_[key.first].name);
        } else
            for (auto p : rs) {
                if (p != w->second) {
                    if (!preserved[key])
                        return fail("Read of discarded version: " + resources_[key.first].name);
                    dependencies[p].insert(w->second);
                } else
                    for (auto& a : passes_[p].accesses)
                        if (!a.writing && a.version.resource == key.first &&
                            a.version.version == key.second && a.usage != Usage::ResolveSource)
                            return fail("Pass reads its own output: " + passes_[p].name);
            }
    }
    // Versions share a physical object. All older readers finish before overwrite.
    for (auto& [key, p] : writers) {
        for (auto& [old, q] : writers)
            if (old.first == key.first && old.second < key.second && q != p)
                dependencies[p].insert(q);
        for (auto& [old, rs] : readers)
            if (old.first == key.first && old.second < key.second)
                for (auto q : rs)
                    if (q != p)
                        dependencies[p].insert(q);
    }
    std::vector<bool> live(passes_.size());
    std::vector<PassId> stack;
    for (PassId p = 0; p < passes_.size(); ++p)
        if (passes_[p].sideEffect) {
            live[p] = true;
            stack.push_back(p);
        }
    while (!stack.empty()) {
        auto p = stack.back();
        stack.pop_back();
        for (auto q : dependencies[p])
            if (!live[q]) {
                live[q] = true;
                stack.push_back(q);
            }
    }
    std::vector<std::vector<PassId>> outgoing(passes_.size());
    std::vector<std::size_t> pending(passes_.size());
    std::size_t liveCount = 0;
    for (PassId p = 0; p < passes_.size(); ++p)
        if (live[p]) {
            ++liveCount;
            for (auto q : dependencies[p]) {
                outgoing[q].push_back(p);
                ++pending[p];
            }
        }
    std::deque<PassId> ready;
    for (PassId p = 0; p < passes_.size(); ++p)
        if (live[p] && !pending[p])
            ready.push_back(p);
    while (!ready.empty()) {
        auto p = ready.front();
        ready.pop_front();
        compiled_.push_back({p, {dependencies[p].begin(), dependencies[p].end()}});
        for (auto q : outgoing[p])
            if (!--pending[q])
                ready.push_back(q);
    }
    if (compiled_.size() != liveCount)
        return fail("Frame graph dependency cycle");
    for (ResourceId id = 0; id < resources_.size(); ++id) {
        std::uint32_t first = noSlot, last = 0;
        for (std::uint32_t i = 0; i < compiled_.size(); ++i)
            for (auto& a : passes_[compiled_[i].id].accesses)
                if (a.version.resource == id) {
                    first = std::min(first, i);
                    last = std::max(last, i);
                }
        if (first != noSlot)
            lifetimes_.push_back({id, first, last, noSlot, resources_[id].imported});
    }
    std::stable_sort(lifetimes_.begin(), lifetimes_.end(),
                     [](auto& a, auto& b) { return a.first < b.first; });
    std::vector<std::uint32_t> slotEnds;
    for (auto& life : lifetimes_)
        if (!life.imported) {
            const auto& desc = resources_[life.resource].desc;
            for (std::uint32_t s = 0; s < slots_.size(); ++s)
                if (slotEnds[s] < life.first && slots_[s] == desc) {
                    life.slot = s;
                    slotEnds[s] = life.last;
                    break;
                }
            if (life.slot == noSlot) {
                life.slot = std::uint32_t(slots_.size());
                slots_.push_back(desc);
                slotEnds.push_back(life.last);
            }
        }
    dirty_ = false;
    return true;
}
bool FrameGraph::execute() {
    if (dirty_ && !compile())
        return false;
    for (auto& p : compiled_)
        if (passes_[p.id].work)
            passes_[p.id].work();
    return true;
}
} // namespace engine::graphics
