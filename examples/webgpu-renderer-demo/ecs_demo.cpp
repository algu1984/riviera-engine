#include "ecs_demo.hpp"

#include <engine/ecs/system_scheduler.hpp>
#include <engine/math.hpp>
#include <engine/scene/hierarchy.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace engine::demo {
namespace {
using Entity = engine::ecs::Entity;
using World = engine::ecs::World<engine::scene::LocalTransform, engine::scene::GlobalTransform,
                                 engine::scene::ChildOf, struct ParentMotion, struct ChildMotion,
                                 struct MeshInstance>;
using Clock = std::chrono::steady_clock;
constexpr std::size_t MaxEntities = 100000;
constexpr std::size_t MaxDraws = 4096;

struct ParentMotion {
    float angle = 0;
    float angularSpeed = 0;
    float radius = 0;
    float phase = 0;
};
struct ChildMotion {
    float angle = 0;
    float angularSpeed = 0;
    float radius = 0;
    float height = 0;
};
struct MeshInstance {
    int mesh = -1;
    float size = .5f;
};

double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct TaskProbe {
    std::atomic<unsigned> active{0};
    std::atomic<unsigned> peak{0};
    std::atomic<std::size_t> calls{0};

    class Guard {
      public:
        explicit Guard(TaskProbe& probe) : probe_(probe) {
            probe_.calls.fetch_add(1, std::memory_order_relaxed);
            const auto now = probe_.active.fetch_add(1, std::memory_order_relaxed) + 1;
            auto seen = probe_.peak.load(std::memory_order_relaxed);
            while (seen < now &&
                   !probe_.peak.compare_exchange_weak(seen, now, std::memory_order_relaxed)) {
            }
        }
        ~Guard() {
            probe_.active.fetch_sub(1, std::memory_order_relaxed);
        }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

      private:
        TaskProbe& probe_;
    };

    Guard track() {
        return Guard(*this);
    }
};

class EcsWorkload final : public Workload {
  public:
    EcsWorkload(engine::jobs::Executor& executor, MeshIds meshes)
        : executor_(executor), meshes_(meshes), hierarchyScratch_(256) {
        scheduler_.add("Animate parent orbits", [this](engine::ecs::Write<ParentMotion> parents) {
            auto guard = probe_.track();
            parents.for_each([this](Entity, ParentMotion& parent) {
                parent.angle += parent.angularSpeed * delta_;
            });
        });
        const auto partitions = std::clamp(executor_.workerCount() + 1U, 2U, 8U);
        scheduler_.add_parallel<ChildMotion>(
            "Animate child orbits", partitions,
            [this](engine::ecs::Range range, engine::ecs::Write<ChildMotion> children) {
                auto guard = probe_.track();
                children.for_each_range(range, [this](Entity, ChildMotion& child) {
                    child.angle += child.angularSpeed * delta_;
                });
            });
    }

    bool configure(std::size_t count) override {
        count = std::min(count, MaxEntities);
        const auto wantedParents = (count + 1) / 2;
        const auto wantedChildren = count / 2;

        while (pairs_.size() > wantedParents) {
            auto& pair = pairs_.back();
            if (pair.child)
                (void)world_.despawn(pair.child);
            (void)world_.despawn(pair.parent);
            pairs_.pop_back();
        }
        while (pairs_.size() < wantedParents) {
            const auto index = pairs_.size();
            Pair pair;
            pair.parent = world_.create();
            if (!pair.parent || !world_.emplace<engine::scene::LocalTransform>(pair.parent) ||
                !world_.emplace<engine::scene::GlobalTransform>(pair.parent) ||
                !world_.emplace<engine::scene::ChildOf>(pair.parent, engine::scene::ChildOf{}) ||
                !world_.emplace<ParentMotion>(
                    pair.parent, ParentMotion{float(index) * .31f, .2f + float(index % 11) * .013f,
                                              4.f + std::sqrt(float(index + 1)) * .32f,
                                              float(index) * 2.39996323f}) ||
                !world_.emplace<MeshInstance>(pair.parent,
                                              MeshInstance{meshes_.spheres[index % 4], .62f}))
                return false;
            pairs_.push_back(pair);
        }

        for (std::size_t i = 0; i < pairs_.size(); ++i) {
            auto& pair = pairs_[i];
            const bool needsChild = i < wantedChildren;
            if (needsChild && !pair.child) {
                pair.child = world_.create();
                if (!pair.child || !world_.emplace<engine::scene::LocalTransform>(pair.child) ||
                    !world_.emplace<engine::scene::GlobalTransform>(pair.child) ||
                    !world_.emplace<engine::scene::ChildOf>(pair.child,
                                                            engine::scene::ChildOf{pair.parent}) ||
                    !world_.emplace<ChildMotion>(
                        pair.child, ChildMotion{float(i) * .37f, .7f + float(i % 9) * .021f,
                                                .9f + float(i % 5) * .08f, float(i % 7) * .12f}) ||
                    !world_.emplace<MeshInstance>(pair.child,
                                                  MeshInstance{meshes_.cubes[(i + 1) % 4], .32f}))
                    return false;
            } else if (!needsChild && pair.child) {
                if (!world_.despawn(pair.child))
                    return false;
                pair.child = {};
            }
        }

        count_ = count;
        metrics_.entities = world_.count<engine::scene::LocalTransform>();
        return metrics_.entities == count_;
    }

    bool step(float deltaSeconds) override {
        if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0)
            return false;
        const auto beforeHelpers = executor_.workerCompletedCount();
        const auto started = Clock::now();
        delta_ = deltaSeconds;
        probe_.calls.store(0, std::memory_order_relaxed);
        probe_.peak.store(0, std::memory_order_relaxed);
        const auto result = scheduler_.run(world_, executor_.scheduler());
        if (!result.success)
            return false;

        world_.for_each<engine::scene::LocalTransform, ParentMotion>(
            [](Entity, engine::scene::LocalTransform& local, const ParentMotion& parent) {
                const auto position = engine::V3{std::sin(parent.angle) * parent.radius,
                                                 .12f * std::sin(parent.phase + parent.angle),
                                                 std::cos(parent.angle) * parent.radius};
                local.matrix =
                    engine::translate(position) * engine::rotateY(parent.phase + parent.angle);
            });
        world_.for_each<engine::scene::LocalTransform, ChildMotion>(
            [](Entity, engine::scene::LocalTransform& local, const ChildMotion& child) {
                const auto position = engine::V3{std::sin(child.angle) * child.radius, child.height,
                                                 std::cos(child.angle) * child.radius};
                local.matrix = engine::translate(position) * engine::rotateY(-child.angle);
            });
        engine::scene::propagate_transforms(world_, hierarchyScratch_);

        metrics_.stepMilliseconds = milliseconds(started);
        metrics_.ticks++;
        metrics_.systems = 2;
        metrics_.tasks = probe_.calls.load(std::memory_order_relaxed);
        metrics_.peakConcurrentTasks = probe_.peak.load(std::memory_order_relaxed);
        metrics_.helperCompletedDelta = executor_.workerCompletedCount() - beforeHelpers;
        return true;
    }

    void extract(std::vector<engine::graphics::Draw>& output) override {
        const auto started = Clock::now();
        output.clear();
        if (output.capacity() < MaxDraws)
            output.reserve(MaxDraws);
        std::uint64_t checksum = 1469598103934665603ULL;
        world_.for_each<engine::scene::GlobalTransform, MeshInstance>(
            [&output, &checksum](Entity entity, const engine::scene::GlobalTransform& global,
                                 const MeshInstance& mesh) {
                if (!global.valid || output.size() >= MaxDraws)
                    return;
                const auto transform =
                    global.matrix * engine::scale({mesh.size, mesh.size, mesh.size});
                output.push_back({mesh.mesh, transform});
                checksum ^= entity.value;
                checksum *= 1099511628211ULL;
                for (float value : {transform.m[12], transform.m[13], transform.m[14]}) {
                    const auto quantized =
                        static_cast<std::int64_t>(std::llround(value * 10000.0f));
                    checksum ^= static_cast<std::uint64_t>(quantized);
                    checksum *= 1099511628211ULL;
                }
            });
        metrics_.visibleDraws = output.size();
        metrics_.checksum = checksum;
        metrics_.extractionMilliseconds = milliseconds(started);
    }

    WorkloadMetrics metrics() const override {
        auto result = metrics_;
        result.entities = world_.count<engine::scene::LocalTransform>();
        return result;
    }

  private:
    struct Pair {
        Entity parent{};
        Entity child{};
    };

    engine::jobs::Executor& executor_;
    MeshIds meshes_;
    World world_;
    engine::ecs::SystemScheduler<World> scheduler_;
    engine::scene::HierarchyScratch hierarchyScratch_;
    std::vector<Pair> pairs_;
    TaskProbe probe_;
    WorkloadMetrics metrics_;
    std::size_t count_ = 0;
    float delta_ = 0;
};
} // namespace

std::unique_ptr<Workload> createEcsWorkload(engine::jobs::Executor& executor, const MeshIds& meshes,
                                            std::size_t count) {
    auto workload = std::make_unique<EcsWorkload>(executor, meshes);
    if (!workload->configure(count))
        return {};
    return workload;
}

} // namespace engine::demo
