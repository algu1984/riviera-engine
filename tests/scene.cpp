#include "engine/scene/scene.hpp"
#include <cassert>
#include <cmath>
#include <vector>

using engine::M4;
using engine::V3;
using engine::scene::DrawScratch;
using engine::scene::PropagationScratch;
using engine::scene::Scene;

static bool near(float a, float b) {
    return std::abs(a - b) < 1e-5f;
}
static void same(const M4& a, const M4& b) {
    for (int i = 0; i < 16; ++i)
        assert(near(a.m[i], b.m[i]));
}

static void hierarchy_and_children() {
    Scene scene;
    PropagationScratch scratch(8);
    auto parent = scene.create_entity(engine::translate({4, 2, -3}) * engine::rotateY(.4f));
    auto child = scene.create_mesh(0, engine::translate({1, 0, 2}), parent);
    auto other = scene.create_mesh(1, engine::translate({0, 3, 0}), parent);
    assert(parent && child && other);
    std::vector<engine::scene::Entity> children;
    assert(scene.children(parent, [&](auto entity) { children.push_back(entity); }) == 2);
    assert(children.size() == 2);

    auto next = scene.create_entity(engine::translate({-8, 0, 5}));
    assert(next && scene.set_parent(child, next));
    assert(!scene.set_parent(next, child)); // obvious cycle rejected synchronously
    assert(scene.children(parent, [&](auto entity) { assert(entity == other); }) == 1);

    auto result = scene.propagate_transforms(scratch);
    assert(result.transformed == 4 && result.cycles == 0);
    engine::scene::MeshBounds bounds[]{{{}, 0.f}, {{}, 0.f}};
    std::vector<engine::graphics::Draw> draws;
    draws.reserve(8);
    DrawScratch drawScratch(8);
    scene.extract_draws(bounds, 2, {}, drawScratch, draws);
    assert(draws.size() == 2);
    for (const auto& draw : draws) {
        if (draw.mesh == 0)
            same(draw.transform, engine::translate({-8, 0, 5}) * engine::translate({1, 0, 2}));
        else
            same(draw.transform, engine::translate({4, 2, -3}) * engine::rotateY(.4f) *
                                     engine::translate({0, 3, 0}));
    }
}

static void nested_transform_composes() {
    const V3 position{12, 1.5f, -7};
    const M4 rotation = engine::rotateY(.72f) * engine::rotateZ(-.08f);
    const V3 offset{-.84f, .31f, 1.24f};
    const M4 expected = engine::translate(position) * rotation * engine::translate(offset) *
                        engine::rotateY(-.27f) * engine::rotateX(.63f) *
                        engine::translate(offset * -1.f);

    Scene scene;
    PropagationScratch scratch(2);
    auto root = scene.create_entity(engine::translate(position) * rotation);
    auto child = scene.create_mesh(0,
                                   engine::translate(offset) * engine::rotateY(-.27f) *
                                       engine::rotateX(.63f) * engine::translate(offset * -1.f),
                                   root);
    assert(root && child);
    scene.propagate_transforms(scratch);
    engine::scene::MeshBounds bounds[]{{{}, 0.f}};
    std::vector<engine::graphics::Draw> draws;
    draws.reserve(1);
    DrawScratch drawScratch(1);
    scene.extract_draws(bounds, 1, {}, drawScratch, draws);
    assert(draws.size() == 1);
    same(draws[0].transform, expected);
}

static void stale_parent_and_cycle_are_bounded() {
    Scene scene;
    PropagationScratch scratch(4);
    auto parent = scene.create_entity(engine::translate({100, 0, 0}));
    auto child = scene.create_mesh(0, engine::translate({2, 0, 0}), parent);
    assert(parent && child && scene.despawn(parent));
    auto result = scene.propagate_transforms(scratch);
    assert(result.staleParents == 1 && result.transformed == 1);
    engine::scene::MeshBounds bounds[]{{{}, 0.f}};
    std::vector<engine::graphics::Draw> draws;
    draws.reserve(4);
    DrawScratch drawScratch(4);
    scene.extract_draws(bounds, 1, {}, drawScratch, draws);
    assert(draws.size() == 1);
    same(draws[0].transform, engine::translate({2, 0, 0})); // stale parent deliberately roots child

    auto a = scene.create_mesh(0, engine::translate({1, 0, 0}), {});
    auto b = scene.create_mesh(0, engine::translate({0, 2, 0}), {});
    assert(a && b);
    assert(scene.set_parent_unchecked(a, b) && scene.set_parent_unchecked(b, a));
    result = scene.propagate_transforms(scratch);
    assert(result.cycles > 0 && result.transformed == 1);
    scene.extract_draws(bounds, 1, {}, drawScratch, draws);
    assert(draws.size() == 1); // invalid cyclic transforms are not submitted
}

static void scratch_growth_and_creation_order() {
    Scene scene;
    PropagationScratch scratch(1);
    std::vector<engine::scene::Entity> entities;
    entities.reserve(320);
    auto parent = scene.create_entity(engine::translate({1, 0, 0}));
    assert(parent);
    entities.push_back(parent);
    for (int i = 1; i < 320; ++i) {
        auto entity = scene.create_mesh(i % 2, engine::translate({0, 1, 0}), entities.back());
        assert(entity);
        entities.push_back(entity);
    }
    const auto result = scene.propagate_transforms(scratch);
    assert(result.transformed == entities.size() && result.cycles == 0);
    auto leaf = scene.ecs().template get<engine::scene::GlobalTransform>(entities.back());
    assert(leaf && leaf->valid);
    same(leaf->matrix, engine::translate({1, 319, 0}));

    engine::scene::MeshBounds bounds[]{{{}, 0.f}, {{}, 0.f}};
    std::vector<engine::graphics::Draw> draws;
    DrawScratch drawScratch(1);
    scene.extract_draws(bounds, 2, {}, drawScratch, draws);
    assert(draws.size() == entities.size() - 1);
    for (std::size_t i = 1; i < draws.size(); ++i)
        assert((draws[i - 1].mesh == 1 && draws[i].mesh == 0) ||
               (draws[i - 1].mesh == 0 && draws[i].mesh == 1));

    assert(scene.despawn(entities[150]));
    const auto afterRemoval = scene.propagate_transforms(scratch);
    assert(afterRemoval.staleParents > 0);
    auto detached = scene.ecs().template get<engine::scene::GlobalTransform>(entities[151]);
    assert(detached && detached->valid);
    same(detached->matrix, engine::translate({0, 1, 0}));
}

int main() {
    hierarchy_and_children();
    nested_transform_composes();
    stale_parent_and_cycle_are_bounded();
    scratch_growth_and_creation_order();
}
