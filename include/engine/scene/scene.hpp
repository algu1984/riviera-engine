#pragma once

#include <engine/graphics/render_object.hpp>
#include <engine/scene/hierarchy.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::scene {

struct MeshInstance {
    int mesh = -1;
};
struct Visibility {
    bool visible = true;
    float drawDistance = -1.f;
};

struct MeshBounds {
    V3 center{};
    float radius = 0.f;
};

using PropagationScratch = HierarchyScratch;

// Draw extraction has its own reusable storage so independent extraction
// tasks never share a mutable Scene-owned buffer.
class DrawScratch {
  public:
    explicit DrawScratch(std::size_t initialReserve = 0);
    DrawScratch(const DrawScratch&) = delete;
    DrawScratch& operator=(const DrawScratch&) = delete;

  private:
    friend class Scene;
    struct Candidate {
        std::uint64_t entity;
        engine::graphics::Draw draw;
    };
    std::vector<Candidate> candidates_;
};

// Scene owns its ECS pools. Scene mutations and queries must not overlap; in
// particular, structural changes must be quiescent with respect to systems
// and queries. Each propagation task supplies its own scratch.
class Scene {
  public:
    using World =
        engine::ecs::World<LocalTransform, GlobalTransform, ChildOf, MeshInstance, Visibility>;

    Scene();
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    Entity create_entity(const M4& local = M4::identity(), Entity parent = {});
    Entity create_mesh(int mesh, const M4& local, Entity parent, float drawDistance = -1.f,
                       bool visible = true);
    bool set_local(Entity entity, const M4& local);
    bool set_parent(Entity child, Entity parent);
    // Skip cycle checking when the caller guarantees the hierarchy is valid.
    // Propagation detects and skips cyclic chains.
    bool set_parent_unchecked(Entity child, Entity parent);
    bool despawn(Entity entity);
    bool set_visible(Entity entity, bool visible);
    bool set_local_mesh_visibility(const std::vector<Entity>& entities, bool visible);
    World& ecs() {
        return world_;
    }
    const World& ecs() const {
        return world_;
    }

    // Visit direct children while the scene structure is quiescent.
    template <class Visitor> std::size_t children(Entity parent, Visitor&& visitor) const {
        std::size_t count = 0;
        world_.template for_each<ChildOf>([&](Entity entity, const ChildOf& relationship) {
            if (relationship.parent == parent) {
                ++count;
                visitor(entity);
            }
        });
        return count;
    }

    PropagationResult propagate_transforms(PropagationScratch& scratch);
    void extract_draws(const MeshBounds* bounds, std::size_t boundsCount, V3 eye,
                       DrawScratch& scratch, std::vector<engine::graphics::Draw>& output) const;

  private:
    bool valid(Entity entity) const;

    World world_;
};

} // namespace engine::scene
