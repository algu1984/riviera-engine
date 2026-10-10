#include "engine/scene/scene.hpp"
#include <algorithm>
#include <unordered_map>

namespace engine::scene {

DrawScratch::DrawScratch(std::size_t initialReserve) {
    candidates_.reserve(initialReserve);
}

Scene::Scene() = default;

bool Scene::valid(Entity entity) const {
    return entity && static_cast<bool>(world_.template get<ChildOf>(entity));
}

Entity Scene::create_entity(const M4& local, Entity parent) {
    if (parent && !valid(parent))
        return {};
    Entity entity = world_.create();
    if (!entity)
        return {};
    if (!world_.template emplace<LocalTransform>(entity) ||
        !world_.template emplace<GlobalTransform>(entity) ||
        !world_.template emplace<ChildOf>(entity) ||
        !world_.template emplace<MeshInstance>(entity) ||
        !world_.template emplace<Visibility>(entity)) {
        world_.despawn(entity);
        return {};
    }

    // Every emplace may move its component pool, so only acquire pointers once
    // all structural changes are complete.
    auto* localTransform = world_.template get<LocalTransform>(entity);
    auto* relationship = world_.template get<ChildOf>(entity);
    if (!localTransform || !relationship) {
        world_.despawn(entity);
        return {};
    }
    localTransform->matrix = local;
    relationship->parent = parent;
    return entity;
}

Entity Scene::create_mesh(int mesh, const M4& local, Entity parent, float drawDistance,
                          bool visible) {
    if (mesh < 0)
        return {};
    Entity entity = create_entity(local, parent);
    if (!entity)
        return {};
    auto instance = world_.template get<MeshInstance>(entity);
    auto visibility = world_.template get<Visibility>(entity);
    if (!instance || !visibility) {
        world_.despawn(entity);
        return {};
    }
    instance->mesh = mesh;
    visibility->drawDistance = drawDistance;
    visibility->visible = visible;
    return entity;
}

bool Scene::set_local(Entity entity, const M4& local) {
    auto transform = world_.template get<LocalTransform>(entity);
    if (!transform)
        return false;
    transform->matrix = local;
    return true;
}

bool Scene::set_parent(Entity child, Entity parent) {
    auto relationship = world_.template get<ChildOf>(child);
    if (!relationship || (parent && !valid(parent)) || child == parent)
        return false;

    // Snapshot edges so cycle checking has a stable view of the hierarchy.
    std::unordered_map<std::uint64_t, std::uint64_t> parents;
    world_.template for_each<ChildOf>([&](Entity entity, const ChildOf& relation) {
        parents.emplace(entity.value, relation.parent.value);
    });
    Entity current = parent;
    std::size_t steps = 0;
    for (; current && steps < parents.size(); ++steps) {
        if (current == child)
            return false;
        auto it = parents.find(current.value);
        if (it == parents.end() || !it->second)
            break;
        current = Entity{it->second};
    }
    if (current && steps == parents.size())
        return false;
    relationship->parent = parent;
    return true;
}

bool Scene::set_parent_unchecked(Entity child, Entity parent) {
    auto relationship = world_.template get<ChildOf>(child);
    if (!relationship || (parent && !valid(parent)))
        return false;
    relationship->parent = parent;
    return true;
}

bool Scene::despawn(Entity entity) {
    return world_.despawn(entity);
}

bool Scene::set_visible(Entity entity, bool visible) {
    auto state = world_.template get<Visibility>(entity);
    if (!state)
        return false;
    state->visible = visible;
    return true;
}

bool Scene::set_local_mesh_visibility(const std::vector<Entity>& entities, bool visible) {
    bool complete = true;
    for (Entity entity : entities)
        complete = set_visible(entity, visible) && complete;
    return complete;
}

PropagationResult Scene::propagate_transforms(PropagationScratch& scratch) {
    return engine::scene::propagate_transforms(world_, scratch);
}

void Scene::extract_draws(const MeshBounds* bounds, std::size_t boundsCount, V3 eye,
                          DrawScratch& scratch, std::vector<engine::graphics::Draw>& output) const {
    auto& candidates = scratch.candidates_;
    candidates.clear();
    world_.template for_each<MeshInstance, GlobalTransform, Visibility>(
        [&](Entity entity, const MeshInstance& instance, const GlobalTransform& transform,
            const Visibility& visibility) {
            if (!visibility.visible)
                return;
            if (!transform.valid)
                return;
            const int meshId = instance.mesh;
            if (meshId < 0 || static_cast<std::size_t>(meshId) >= boundsCount || !bounds)
                return;
            const MeshBounds& mesh = bounds[meshId];
            const float distance = visibility.drawDistance;
            const M4 worldTransform = transform.matrix;
            if (distance >= 0.f &&
                length(point(worldTransform, mesh.center) - eye) >= distance + mesh.radius)
                return;
            candidates.push_back({entity.value, {meshId, worldTransform}});
        });
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.entity < b.entity; });
    output.clear();
    output.reserve(candidates.size());
    for (auto& candidate : candidates)
        output.push_back(candidate.draw);
}

} // namespace engine::scene
