#include "engine/physics/physics_world.hpp"

#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>

namespace engine::physics {

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
    : temporaryAllocator_(settings.temporaryAllocatorBytes), jobs_(settings.maxJobs) {
    system_.Init(settings.maxBodies, settings.bodyMutexCount, settings.maxBodyPairs,
                 settings.maxContactConstraints, layers, broadPhaseFilter, pairFilter);
}

JPH::EPhysicsUpdateError PhysicsWorld::step(float deltaTime, int collisionSteps) {
    return system_.Update(deltaTime, collisionSteps, &temporaryAllocator_, &jobs_);
}

} // namespace engine::physics
