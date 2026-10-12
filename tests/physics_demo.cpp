#include "../examples/webgpu-renderer-demo/physics_demo.hpp"

#include <engine/physics/physics_world.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

namespace {
engine::demo::MeshIds testMeshes() {
    engine::demo::MeshIds meshes;
    meshes.cubes = {0, 1, 2, 3};
    meshes.spheres = {4, 5, 6, 7};
    return meshes;
}

std::vector<engine::graphics::Draw> settle(engine::jobs::Executor& executor, std::size_t count) {
    auto workload = engine::demo::createPhysicsWorkload(executor, testMeshes(), count);
    assert(workload);
    for (int i = 0; i < 480; ++i)
        assert(workload->step(1.f / 60.f));
    std::vector<engine::graphics::Draw> draws;
    workload->extract(draws);
    return draws;
}
} // namespace

int main() {
    engine::physics::initialize_jolt();

    engine::jobs::Executor serial(0);
    const auto serialDraws = settle(serial, 16);
    assert(serialDraws.size() == 17); // 16 simulated boxes and one rendered floor.
    float lowestCenter = 1000.f;
    for (std::size_t i = 1; i < serialDraws.size(); ++i) {
        const auto& transform = serialDraws[i].transform;
        const float centerY = transform.m[13];
        assert(std::isfinite(transform.m[12]) && std::isfinite(centerY) &&
               std::isfinite(transform.m[14]));
        lowestCenter = std::min(lowestCenter, centerY);
    }
    assert(lowestCenter >= .43f); // Boxes settle on the top of the static floor.

    engine::jobs::Executor helpers(2);
    const auto helperDraws = settle(helpers, 16);
    assert(helperDraws.size() == serialDraws.size());
    for (std::size_t i = 1; i < serialDraws.size(); ++i) {
        for (int axis = 12; axis <= 14; ++axis)
            assert(std::abs(serialDraws[i].transform.m[axis] - helperDraws[i].transform.m[axis]) <
                   .08f);
    }

    auto capacity = engine::demo::createPhysicsWorkload(helpers, testMeshes(), 1000);
    assert(capacity);
    assert(capacity->metrics().entities == 1000);
    assert(capacity->step(1.f / 120.f));
    std::vector<engine::graphics::Draw> largeDraws;
    capacity->extract(largeDraws);
    assert(largeDraws.size() == 1001);
    assert(capacity->metrics().checksum != 0);
    assert(capacity->configure(4000)); // The public limit is 3999 dynamic bodies.
    assert(capacity->metrics().entities == 3999);
    assert(capacity->step(1.f / 120.f));
    capacity->extract(largeDraws);
    assert(largeDraws.size() == 4000);
    float minimumY = 1000.f;
    float maximumY = -1000.f;
    for (std::size_t i = 1; i < largeDraws.size(); ++i) {
        const auto& draw = largeDraws[i];
        for (float value : draw.transform.m)
            assert(std::isfinite(value));
        minimumY = std::min(minimumY, draw.transform.m[13]);
        maximumY = std::max(maximumY, draw.transform.m[13]);
    }
    assert(minimumY >= .43f && maximumY < 9.f);
}
