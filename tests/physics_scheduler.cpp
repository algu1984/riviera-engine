#include "engine/jobs/executor.hpp"
#include "engine/jobs/task_graph.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/physics/scheduler_job_system.hpp"

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

namespace {
struct CleanupGate {
    explicit CleanupGate(std::atomic<bool>& finished) : finished(finished) {}
    ~CleanupGate() {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        finished.store(true, std::memory_order_release);
    }
    std::atomic<bool>& finished;
};

void verify_scheduler_jobs(engine::jobs::Executor& executor) {
    engine::physics::SchedulerJobSystem jobs(executor.scheduler(), 256,
                                             static_cast<int>(executor.workerCount() + 1));
    std::atomic<unsigned> completed{0};
    std::atomic<bool> cleanupFinished{false};

    // The child is created by a running Jolt job and joined through the same
    // barrier while WaitForJobs helps the Engine's shared queue.
    auto* dynamicBarrier = jobs.CreateBarrier();
    auto parent = jobs.CreateJob(
        "dynamic parent", JPH::Color::sGreen,
        [&] {
            auto child = jobs.CreateJob(
                "dynamic child", JPH::Color::sBlue, [&] { completed.fetch_add(1); }, 1);
            dynamicBarrier->AddJob(child);
            child.RemoveDependency();
            completed.fetch_add(1);
        },
        1);
    dynamicBarrier->AddJob(parent);
    parent.RemoveDependency();
    jobs.WaitForJobs(dynamicBarrier);
    assert(completed.load() == 2);
    jobs.DestroyBarrier(dynamicBarrier);

    // Queue an Engine task beside Jolt work. A simulation-owner task waits by
    // helping the scheduler instead of calling Executor::run recursively.
    std::atomic<bool> independentTaskRan{false};
    engine::jobs::TaskGraph independent;
    independent.add("independent shared-queue task", [&] {
        independentTaskRan.store(true, std::memory_order_release);
        return true;
    });
    (void)executor.submit(independent);

    engine::jobs::TaskGraph simulationOwner;
    simulationOwner.add("simulation owner", [&] {
        auto* barrier = jobs.CreateBarrier();
        auto ownerJob = jobs.CreateJob(
            "simulation-owned physics job", JPH::Color::sRed, [&] { completed.fetch_add(1); }, 1);
        barrier->AddJob(ownerJob);
        ownerJob.RemoveDependency();
        jobs.WaitForJobs(barrier);
        jobs.DestroyBarrier(barrier);
        return true;
    });
    auto ownerTicket = executor.submit(simulationOwner);
    for (;;) {
        const auto status = executor.poll(ownerTicket);
        if (status.complete) {
            assert(status.success);
            break;
        }
        if (!executor.helpOne())
            std::this_thread::yield();
    }
    assert(independentTaskRan.load(std::memory_order_acquire));
    assert(completed.load() == 3);

    // A completed job added later must be rejected without changing the
    // barrier count. Waiting for IsDone before AddJob makes this deterministic.
    for (int iteration = 0; iteration < 64; ++iteration) {
        auto* barrier = jobs.CreateBarrier();
        auto fast = jobs.CreateJob(
            "fast barrier job", JPH::Color::sYellow, [&] { completed.fetch_add(1); }, 1);
        fast.RemoveDependency();
        while (!fast.IsDone()) {
            if (!executor.helpOne())
                std::this_thread::yield();
        }
        barrier->AddJob(fast);
        jobs.WaitForJobs(barrier);
        jobs.DestroyBarrier(barrier);
    }

    // OnJobFinished runs inside Job::Execute, before the queue reference is
    // released. The captured object's delayed destruction must finish before
    // WaitForJobs returns.
    auto* cleanupBarrier = jobs.CreateBarrier();
    auto gate = std::make_shared<CleanupGate>(cleanupFinished);
    auto cleanupJob = jobs.CreateJob(
        "cleanup lifetime", JPH::Color::sWhite, [gate, &completed] { completed.fetch_add(1); }, 1);
    cleanupBarrier->AddJob(cleanupJob);
    cleanupJob.RemoveDependency();
    cleanupJob = {};
    gate.reset();
    jobs.WaitForJobs(cleanupBarrier);
    assert(cleanupFinished.load(std::memory_order_acquire));
    jobs.DestroyBarrier(cleanupBarrier);
}

float simulate_checksum(engine::jobs::Executor* executor,
                        const JPH::BroadPhaseLayerInterface& layers,
                        const JPH::ObjectVsBroadPhaseLayerFilter& broadPhaseFilter,
                        const JPH::ObjectLayerPairFilter& pairs, const JPH::Shape* shape) {
    engine::physics::PhysicsWorldSettings settings;
    settings.maxBodies = 128;
    settings.maxBodyPairs = 1024;
    settings.maxContactConstraints = 1024;
    settings.maxJobs = 512;
    settings.executor = executor;
    engine::physics::PhysicsWorld world(layers, broadPhaseFilter, pairs, settings);

    std::vector<JPH::BodyID> bodies;
    for (int index = 0; index < 40; ++index) {
        const float x = static_cast<float>(index % 5) * 1.5f;
        const float y = 4.0f + static_cast<float>(index / 5) * 1.5f;
        JPH::BodyCreationSettings bodySettings(shape, JPH::RVec3(x, y, 0), JPH::Quat::sIdentity(),
                                               JPH::EMotionType::Dynamic, 0);
        const auto body =
            world.body_interface().CreateAndAddBody(bodySettings, JPH::EActivation::Activate);
        assert(!body.IsInvalid());
        bodies.push_back(body);
    }

    for (int step = 0; step < 120; ++step)
        assert(world.step(1.0f / 60.0f) == JPH::EPhysicsUpdateError::None);

    float checksum = 0.0f;
    for (const auto body : bodies) {
        const auto position = world.body_interface().GetPosition(body);
        checksum +=
            static_cast<float>(position.GetX() + position.GetY() * 3.0 + position.GetZ() * 7.0);
        world.body_interface().RemoveBody(body);
        world.body_interface().DestroyBody(body);
    }
    return checksum;
}
} // namespace

int main() {
    engine::physics::initialize_jolt();
    engine::jobs::Executor executor(3);
    verify_scheduler_jobs(executor);

    JPH::BroadPhaseLayerInterfaceTable layers(1, 1);
    layers.MapObjectToBroadPhaseLayer(0, JPH::BroadPhaseLayer(0));
    JPH::ObjectLayerPairFilterTable pairs(1);
    pairs.EnableCollision(0, 0);
    JPH::ObjectVsBroadPhaseLayerFilterTable broadPhaseFilter(layers, 1, pairs, 1);
    JPH::BoxShapeSettings shapeSettings(JPH::Vec3(0.5f, 0.5f, 0.5f));
    auto shape = shapeSettings.Create();
    assert(!shape.HasError());

    const float serial = simulate_checksum(nullptr, layers, broadPhaseFilter, pairs, shape.Get());
    const auto helperCompletionsBeforeParallel = executor.workerCompletedCount();
    const float parallel =
        simulate_checksum(&executor, layers, broadPhaseFilter, pairs, shape.Get());
    assert(executor.workerCompletedCount() > helperCompletionsBeforeParallel);
    assert(std::abs(serial - parallel) < 0.01f);
}
