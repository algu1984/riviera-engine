#include "engine/physics/physics_world.hpp"

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>

#include <cassert>

int main() {
    engine::physics::initialize_jolt();

    JPH::BroadPhaseLayerInterfaceTable layers(1, 1);
    layers.MapObjectToBroadPhaseLayer(0, JPH::BroadPhaseLayer(0));
    JPH::ObjectLayerPairFilterTable pairs(1);
    pairs.EnableCollision(0, 0);
    JPH::ObjectVsBroadPhaseLayerFilterTable broadPhaseFilter(layers, 1, pairs, 1);

    engine::physics::PhysicsWorldSettings settings;
    settings.maxBodies = 8;
    settings.maxBodyPairs = 8;
    settings.maxContactConstraints = 8;
    engine::physics::PhysicsWorld world(layers, broadPhaseFilter, pairs, settings);

    JPH::BoxShapeSettings shapeSettings(JPH::Vec3(1, 1, 1));
    auto shape = shapeSettings.Create();
    assert(!shape.HasError());
    JPH::BodyCreationSettings bodySettings(shape.Get(), JPH::RVec3(0, 0, 0), JPH::Quat::sIdentity(),
                                           JPH::EMotionType::Static, 0);
    JPH::BodyID body =
        world.body_interface().CreateAndAddBody(bodySettings, JPH::EActivation::DontActivate);
    assert(!body.IsInvalid());
    assert(world.step(1.f / 60.f) == JPH::EPhysicsUpdateError::None);
    world.body_interface().RemoveBody(body);
    world.body_interface().DestroyBody(body);
}
