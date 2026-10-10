#include "engine/ecs/system_scheduler.hpp"
#include "engine/jobs/executor.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

struct Position {
    int value = 0;
};
struct Velocity {
    int value = 0;
};
struct Summary {
    int value = 0;
};

using World = engine::ecs::World<Position, Velocity, Summary>;

static void require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", message);
        std::exit(1);
    }
}

int main() {
    using namespace engine;
    constexpr int entityCount = 256;
    World world;
    int expectedSum = 0;
    for (int i = 0; i < entityCount; ++i) {
        const auto entity = world.create();
        const int velocity = i % 3 + 1;
        require(entity && world.emplace<Position>(entity, Position{}) &&
                    world.emplace<Velocity>(entity, Velocity{velocity}),
                "scene data is prepared before scheduled execution");
        expectedSum += velocity;
    }
    const auto summaryEntity = world.create();
    require(summaryEntity && world.emplace<Summary>(summaryEntity),
            "summary component is available to ordinary systems");

    jobs::Executor executor(2);
    require(executor.workerCount() == 2, "executor owns the requested host workers");
    ecs::SystemScheduler<World> systems;
    std::atomic<int> activeRanges{0};
    std::atomic<int> maximumRanges{0};
    std::atomic<bool> usedHostWorker{false};
    const auto callerThread = std::this_thread::get_id();
    int observedSum = 0;

    const auto update = systems.add_parallel<Position>(
        "parallel update", 4,
        [&](ecs::Range range, ecs::Write<Position> positions, ecs::Read<Velocity> velocities) {
            if (std::this_thread::get_id() != callerThread)
                usedHostWorker.store(true, std::memory_order_relaxed);
            const int active = activeRanges.fetch_add(1, std::memory_order_relaxed) + 1;
            int maximum = maximumRanges.load(std::memory_order_relaxed);
            while (maximum < active && !maximumRanges.compare_exchange_weak(
                                           maximum, active, std::memory_order_relaxed)) {
            }
            positions.for_each_range(range, [&](ecs::Entity entity, Position& position) {
                const auto* velocity = velocities.get(entity);
                require(velocity != nullptr, "read access joins matching entities");
                position.value += velocity->value;
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            activeRanges.fetch_sub(1, std::memory_order_relaxed);
        });
    const auto summarize = systems.add("ordinary read and write", [&](ecs::Read<Position> positions,
                                                                      ecs::Write<Summary> summary) {
        int sum = 0;
        positions.for_each([&](ecs::Entity, const Position& position) { sum += position.value; });
        auto* result = summary.get(summaryEntity);
        require(result != nullptr, "write access finds the summary entity");
        result->value = sum;
    });
    systems.add("ordinary read", [&](ecs::Read<Summary> summary) {
        const auto* result = summary.get(summaryEntity);
        require(result != nullptr, "read access observes the summary entity");
        observedSum = result->value;
    });

    const auto result = systems.run(world, executor.scheduler());
    require(result.success,
            result.error.empty() ? "systems execute on the engine executor" : result.error.c_str());
    require(maximumRanges.load(std::memory_order_relaxed) > 1,
            "range jobs overlap on the executor worker pool");
    require(usedHostWorker.load(std::memory_order_relaxed),
            "range jobs execute on a host worker owned by Executor");
    require(observedSum == expectedSum, "inferred read/write systems observe completed ranges");
    require(world.count<Position>() == entityCount,
            "parallel query partitions process every dense position row");
    int sum = 0;
    world.for_each<Position>([&](ecs::Entity, const Position& position) { sum += position.value; });
    require(sum == expectedSum, "plain component fields update once across disjoint ranges");
    require(summarize != update, "system registration returns distinct identifiers");
}
