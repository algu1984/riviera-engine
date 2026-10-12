#include "jobs_demo.hpp"

#include <engine/ecs/system_scheduler.hpp>
#include <engine/math.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace engine::demo {
namespace {
using Entity = engine::ecs::Entity;
constexpr std::size_t MaxEntities = 100000;
constexpr std::size_t MaxDraws = 4096;
using Clock = std::chrono::steady_clock;

struct Position {
    engine::V3 value{};
    float phase = 0;
    int mesh = -1;
    float size = .5f;
};
struct Velocity {
    engine::V3 linear{};
    float angular = 0;
};
struct Heat {
    float value = .1f;
};
using World = engine::ecs::World<Position, Velocity, Heat>;

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

std::uint64_t quantize(float value) {
    const auto scaled = static_cast<std::int64_t>(std::llround(double(value) * 10000.0));
    return static_cast<std::uint64_t>(scaled);
}

class JobsWorkload final : public Workload {
  public:
    JobsWorkload(engine::jobs::Executor& executor, MeshIds meshes)
        : executor_(executor), meshes_(meshes) {
        const auto partitions = std::clamp(executor_.workerCount() + 1U, 2U, 8U);
        scheduler_.add_parallel<Position>(
            "Integrate independent positions", partitions,
            [this](engine::ecs::Range range, engine::ecs::Write<Position> positions,
                   engine::ecs::Read<Velocity> velocities) {
                auto guard = probe_.track();
                positions.for_each_range(range,
                                         [this, velocities](Entity entity, Position& position) {
                                             const auto* velocity = velocities.get(entity);
                                             if (!velocity)
                                                 return;
                                             position.value += velocity->linear * delta_;
                                             position.phase += velocity->angular * delta_;
                                         });
            });
        scheduler_.add_parallel<Heat>(
            "Run independent heat workload", partitions,
            [this](engine::ecs::Range range, engine::ecs::Write<Heat> temperatures) {
                auto guard = probe_.track();
                temperatures.for_each_range(range, [](Entity entity, Heat& heat) {
                    float value = heat.value + float(entity.index() % 23) * .0007f;
                    for (unsigned iteration = 0; iteration < 96; ++iteration)
                        value = std::sin(value * 1.0001f +
                                         float((entity.index() + iteration) % 31) * .001f);
                    heat.value = value;
                });
            });
        scheduler_.add(
            "Join and checksum completed systems",
            [this](engine::ecs::Read<Position> positions, engine::ecs::Read<Heat> temperatures) {
                auto guard = probe_.track();
                std::uint64_t checksum = 1469598103934665603ULL;
                positions.for_each([&](Entity entity, const Position& position) {
                    const auto* heat = temperatures.get(entity);
                    if (!heat)
                        return;
                    checksum ^= entity.value;
                    checksum *= 1099511628211ULL;
                    for (float value : {position.value.x, position.value.y, position.value.z,
                                        position.phase, heat->value}) {
                        checksum ^= quantize(value);
                        checksum *= 1099511628211ULL;
                    }
                });
                checksum_ = checksum;
            });
    }

    bool configure(std::size_t count) override {
        count = std::min(count, MaxEntities);
        while (entities_.size() > count) {
            if (!world_.despawn(entities_.back()))
                return false;
            entities_.pop_back();
        }
        entities_.reserve(count);
        while (entities_.size() < count) {
            const auto index = entities_.size();
            const auto entity = world_.create();
            if (!entity)
                return false;
            const auto columns =
                std::max<std::size_t>(1, std::size_t(std::ceil(std::sqrt(double(count)))));
            const float x = (float(index % columns) - float(columns - 1) * .5f) * 1.35f;
            const float z =
                (float(index / columns) - float((count + columns - 1) / columns - 1) * .5f) * 1.35f;
            const float phase = float(index) * .173f;
            const float speed = .12f + float(index % 17) * .008f;
            const Position position{{x, 0, z},
                                    phase,
                                    index % 3 == 0 ? meshes_.spheres[index % 4]
                                                   : meshes_.cubes[index % 4],
                                    .42f + float(index % 5) * .035f};
            const Velocity velocity{{-z * speed * .15f, 0, x * speed * .15f}, speed};
            if (!world_.emplace<Position>(entity, position) ||
                !world_.emplace<Velocity>(entity, velocity) ||
                !world_.emplace<Heat>(entity, Heat{.1f + float(index % 19) * .03f}))
                return false;
            entities_.push_back(entity);
        }
        metrics_.entities = entities_.size();
        checksum_ = 0;
        return true;
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
        metrics_.stepMilliseconds = milliseconds(started);
        metrics_.ticks++;
        metrics_.systems = 3;
        metrics_.tasks = probe_.calls.load(std::memory_order_relaxed);
        metrics_.peakConcurrentTasks = probe_.peak.load(std::memory_order_relaxed);
        metrics_.helperCompletedDelta = executor_.workerCompletedCount() - beforeHelpers;
        metrics_.checksum = checksum_;
        return true;
    }

    void extract(std::vector<engine::graphics::Draw>& output) override {
        const auto started = Clock::now();
        output.clear();
        if (output.capacity() < MaxDraws)
            output.reserve(MaxDraws);
        world_.for_each<Position>([&output](Entity, const Position& position) {
            if (output.size() >= MaxDraws)
                return;
            output.push_back({position.mesh,
                              engine::translate(position.value) * engine::rotateY(position.phase) *
                                  engine::scale({position.size, position.size, position.size})});
        });
        metrics_.visibleDraws = output.size();
        metrics_.extractionMilliseconds = milliseconds(started);
    }

    WorkloadMetrics metrics() const override {
        return metrics_;
    }

  private:
    engine::jobs::Executor& executor_;
    MeshIds meshes_;
    World world_;
    engine::ecs::SystemScheduler<World> scheduler_;
    std::vector<Entity> entities_;
    TaskProbe probe_;
    WorkloadMetrics metrics_;
    float delta_ = 0;
    std::uint64_t checksum_ = 0;
};
} // namespace

std::unique_ptr<Workload> createJobsWorkload(engine::jobs::Executor& executor,
                                             const MeshIds& meshes, std::size_t count) {
    auto workload = std::make_unique<JobsWorkload>(executor, meshes);
    if (!workload->configure(count))
        return {};
    return workload;
}

} // namespace engine::demo
