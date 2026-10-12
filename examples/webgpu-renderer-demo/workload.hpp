#pragma once

#include <engine/graphics/render_object.hpp>
#include <engine/jobs/executor.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace engine::demo {

enum class Mode : int { Renderer = 0, Ecs = 1, Jobs = 2, Physics = 3, Pbr = 4 };

struct MeshIds {
    std::array<int, 4> cubes{};
    std::array<int, 4> spheres{};
};

struct WorkloadMetrics {
    std::size_t entities = 0;
    std::size_t visibleDraws = 0;
    std::size_t systems = 0;
    std::size_t tasks = 0;
    std::size_t helperCompletedDelta = 0;
    double stepMilliseconds = 0;
    double extractionMilliseconds = 0;
    std::uint64_t ticks = 0;
    std::uint64_t checksum = 0;
    unsigned peakConcurrentTasks = 0;
};

class Workload {
  public:
    virtual ~Workload() = default;
    virtual bool configure(std::size_t count) = 0;
    virtual bool step(float deltaSeconds) = 0;
    virtual void extract(std::vector<engine::graphics::Draw>& output) = 0;
    virtual WorkloadMetrics metrics() const = 0;
};

std::unique_ptr<Workload> createEcsWorkload(engine::jobs::Executor& executor, const MeshIds& meshes,
                                            std::size_t count);
std::unique_ptr<Workload> createJobsWorkload(engine::jobs::Executor& executor,
                                             const MeshIds& meshes, std::size_t count);
std::unique_ptr<Workload> createPhysicsWorkload(engine::jobs::Executor& executor,
                                                const MeshIds& meshes, std::size_t count);

} // namespace engine::demo
