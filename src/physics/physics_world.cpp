#include "engine/physics/physics_world.hpp"
#include "engine/physics/scheduler_job_system.hpp"

#include "engine/jobs/executor.hpp"

#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cstdint>

namespace engine::physics {
namespace {
JPH::uint normalized_max_jobs(JPH::uint maxJobs) {
    return maxJobs == 0 ? JPH::cMaxPhysicsJobs : maxJobs;
}
} // namespace

void initialize_jolt(JPH::TraceFunction trace) {
    JPH::RegisterDefaultAllocator();
    if (trace)
        JPH::Trace = trace;
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
}

PhysicsWorld::PhysicsWorld(const JPH::BroadPhaseLayerInterface& layers,
                           const JPH::ObjectVsBroadPhaseLayerFilter& broadPhaseFilter,
                           const JPH::ObjectLayerPairFilter& pairFilter,
                           PhysicsWorldSettings settings)
    : temporaryAllocator_(settings.temporaryAllocatorBytes),
      jobs_(normalized_max_jobs(settings.maxJobs)) {
    if (settings.executor) {
        const auto maxJobs = normalized_max_jobs(settings.maxJobs);
        const auto requestedConcurrency =
            static_cast<std::uint64_t>(settings.executor->workerCount()) + 1;
        const auto concurrency =
            static_cast<int>(std::min<std::uint64_t>(requestedConcurrency, maxJobs));
        schedulerJobs_ = std::make_unique<SchedulerJobSystem>(settings.executor->scheduler(),
                                                              maxJobs, concurrency);
    }
    system_.Init(settings.maxBodies, settings.bodyMutexCount, settings.maxBodyPairs,
                 settings.maxContactConstraints, layers, broadPhaseFilter, pairFilter);
}

PhysicsWorld::~PhysicsWorld() = default;

JPH::EPhysicsUpdateError PhysicsWorld::step(float deltaTime, int collisionSteps) {
    auto* jobSystem = schedulerJobs_ ? static_cast<JPH::JobSystem*>(schedulerJobs_.get()) : &jobs_;
    return system_.Update(deltaTime, collisionSteps, &temporaryAllocator_, jobSystem);
}

} // namespace engine::physics
