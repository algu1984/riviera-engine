#pragma once

#include <Jolt/Jolt.h>

#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <cstddef>

namespace engine::physics {

// Call once on the host thread before creating worlds or worker threads.
// This installs Jolt's allocator, factory, types, and optional trace callback.
void initialize_jolt(JPH::TraceFunction trace = nullptr);

struct PhysicsWorldSettings {
    JPH::uint maxBodies = 1024;
    // Zero asks Jolt to select its body-lock count from the hardware.
    JPH::uint bodyMutexCount = 0;
    JPH::uint maxBodyPairs = 1024;
    JPH::uint maxContactConstraints = 1024;
    std::size_t temporaryAllocatorBytes = 4 * 1024 * 1024;
    JPH::uint maxJobs = JPH::cMaxPhysicsJobs;
};

// Owns the reusable Jolt world, temporary allocator, and serial job system.
// The supplied layer interfaces must outlive this object. A world has one
// owner: call step and mutate its bodies on that owner only. Independent worlds
// may be stepped concurrently by the application scheduler.
class PhysicsWorld {
  public:
    PhysicsWorld(const JPH::BroadPhaseLayerInterface& layers,
                 const JPH::ObjectVsBroadPhaseLayerFilter& broadPhaseFilter,
                 const JPH::ObjectLayerPairFilter& pairFilter, PhysicsWorldSettings settings = {});
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    JPH::EPhysicsUpdateError step(float deltaTime, int collisionSteps = 1);
    JPH::PhysicsSystem& system() {
        return system_;
    }
    const JPH::PhysicsSystem& system() const {
        return system_;
    }
    JPH::BodyInterface& body_interface() {
        return system_.GetBodyInterface();
    }

  private:
    JPH::TempAllocatorImpl temporaryAllocator_;
    JPH::JobSystemSingleThreaded jobs_;
    JPH::PhysicsSystem system_;
};

} // namespace engine::physics
