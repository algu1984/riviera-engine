#include "examples/webgpu-renderer-demo/workload.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        std::exit(1);
    }
}

engine::demo::MeshIds meshes() {
    return {{10, 11, 12, 13}, {20, 21, 22, 23}};
}

std::vector<engine::graphics::Draw> runWorkload(engine::demo::Workload& workload) {
    require(workload.step(1.f / 120.f), "workload step succeeds");
    require(workload.step(1.f / 120.f), "repeated workload step succeeds");
    std::vector<engine::graphics::Draw> output;
    workload.extract(output);
    return output;
}

void requireSameDraws(const std::vector<engine::graphics::Draw>& left,
                      const std::vector<engine::graphics::Draw>& right, const char* message) {
    require(left.size() == right.size(), message);
    for (std::size_t i = 0; i < left.size(); ++i) {
        require(left[i].mesh == right[i].mesh, message);
        for (std::size_t component = 0; component < 16; ++component)
            require(std::abs(left[i].transform.m[component] - right[i].transform.m[component]) <
                        1e-6f,
                    message);
    }
}

void ecsWorkload() {
    using namespace engine;
    jobs::Executor serial(0);
    jobs::Executor helpers(2);
    auto first = demo::createEcsWorkload(serial, meshes(), 1000);
    auto second = demo::createEcsWorkload(helpers, meshes(), 1000);
    require(first && second, "ECS workload initializes with the requested population");
    const auto firstDraws = runWorkload(*first);
    const auto secondDraws = runWorkload(*second);
    require(first->metrics().entities == 1000 && firstDraws.size() == 1000,
            "ECS hierarchy exposes its configured entity count");
    requireSameDraws(firstDraws, secondDraws,
                     "serial and helper execution preserve deterministic hierarchy transforms");
    require(first->metrics().checksum == second->metrics().checksum,
            "serial and helper extraction checksums match");

    auto hierarchy = demo::createEcsWorkload(serial, meshes(), 2);
    require(hierarchy != nullptr, "two-object hierarchy initializes");
    require(hierarchy->step(1.f / 120.f), "two-object hierarchy advances");
    std::vector<graphics::Draw> parentChild;
    hierarchy->extract(parentChild);
    require(parentChild.size() == 2, "parent and child both reach draw extraction");
    const auto dx = parentChild[0].transform.m[12] - parentChild[1].transform.m[12];
    const auto dy = parentChild[0].transform.m[13] - parentChild[1].transform.m[13];
    const auto dz = parentChild[0].transform.m[14] - parentChild[1].transform.m[14];
    const auto separation = std::sqrt(dx * dx + dy * dy + dz * dz);
    require(separation > .5f && separation < 2.5f,
            "Core hierarchy propagation places the child relative to its moving parent");

    require(first->configure(100), "ECS workload shrinks at a structural change boundary");
    require(first->metrics().entities == 100, "ECS population reflects the shrink");
    require(first->step(1.f / 120.f), "ECS workload advances after reconfiguration");
    first->extract(parentChild);
    require(parentChild.size() == 100, "shrunk ECS population extracts exactly its live draws");
    require(first->configure(0), "ECS workload accepts an empty population");
    first->extract(parentChild);
    require(parentChild.empty(), "empty ECS population extracts no stale objects");
    require(!first->step(0.f), "invalid simulation delta is rejected");
}

void jobsWorkload() {
    using namespace engine;
    jobs::Executor serial(0);
    jobs::Executor helpers(2);
    auto first = demo::createJobsWorkload(serial, meshes(), 768);
    auto second = demo::createJobsWorkload(helpers, meshes(), 768);
    require(first && second, "jobs workload initializes its entities");
    const auto firstDraws = runWorkload(*first);
    const auto secondDraws = runWorkload(*second);
    require(first->metrics().entities == 768 && firstDraws.size() == 768,
            "jobs workload renders its configured entities");
    require(first->metrics().systems == 3 && first->metrics().tasks > 0,
            "jobs metrics count scheduler systems and executed task callbacks");
    require(first->metrics().checksum == second->metrics().checksum,
            "integer-quantized jobs checksum is stable with helper workers");
    requireSameDraws(firstDraws, secondDraws,
                     "serial and helper execution produce the same jobs workload transforms");
    require(first->configure(96), "jobs workload accepts a smaller population");
    require(first->step(1.f / 120.f), "jobs workload advances after reconfiguration");
    std::vector<graphics::Draw> reduced;
    first->extract(reduced);
    require(reduced.size() == 96, "jobs workload removes entities at the owner boundary");
    require(!first->step(std::numeric_limits<float>::quiet_NaN()),
            "jobs workload rejects a non-finite delta");
}
} // namespace

int main() {
    ecsWorkload();
    jobsWorkload();
    std::puts("PASS demo workloads");
}
