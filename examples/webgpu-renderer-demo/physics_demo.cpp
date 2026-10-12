#include "physics_demo.hpp"

#include <engine/math.hpp>
#include <engine/physics/physics_world.hpp>

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <vector>

namespace engine::demo {
namespace {
constexpr std::size_t MaxDynamicBodies = 3999;
constexpr std::size_t MaxWorldBodies = MaxDynamicBodies + 1;
constexpr double FixedStep = 1.0 / 120.0;
constexpr int CollisionSteps = 2;
constexpr JPH::ObjectLayer StaticLayer = 0;
constexpr JPH::ObjectLayer MovingLayer = 1;
using Clock = std::chrono::steady_clock;

double elapsedMilliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct BoxRecord {
    JPH::BodyID id;
    int mesh = -1;
    float halfExtent = .45f;
};

} // namespace

struct PhysicsWorkloadImpl {
    PhysicsWorkloadImpl(engine::jobs::Executor& executorValue, MeshIds meshValues,
                        std::size_t bodyCount)
        : executor(executorValue), meshes(meshValues), layers(2, 2), pairs(2) {
        layers.MapObjectToBroadPhaseLayer(StaticLayer, JPH::BroadPhaseLayer(0));
        layers.MapObjectToBroadPhaseLayer(MovingLayer, JPH::BroadPhaseLayer(1));
        pairs.EnableCollision(StaticLayer, MovingLayer);
        pairs.EnableCollision(MovingLayer, MovingLayer);
        broadPhaseFilter =
            std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(layers, 2, pairs, 2);
        engine::physics::PhysicsWorldSettings settings;
        settings.maxBodies = static_cast<JPH::uint>(MaxWorldBodies);
        settings.maxBodyPairs = 32768;
        settings.maxContactConstraints = 32768;
        settings.temporaryAllocatorBytes = 16 * 1024 * 1024;
        settings.executor = &executor;
        world = std::make_unique<engine::physics::PhysicsWorld>(layers, *broadPhaseFilter, pairs,
                                                                settings);
        initialized = createFloor() && configure(bodyCount);
    }

    ~PhysicsWorkloadImpl() {
        destroyBodies();
    }

    bool createFloor() {
        JPH::BoxShapeSettings shapeSettings(JPH::Vec3(40.f, .5f, 40.f));
        auto shape = shapeSettings.Create();
        if (shape.HasError())
            return false;
        JPH::BodyCreationSettings settings(shape.Get(), JPH::RVec3(0, -.5, 0),
                                           JPH::Quat::sIdentity(), JPH::EMotionType::Static,
                                           StaticLayer);
        floor = world->body_interface().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
        return !floor.IsInvalid();
    }

    void destroyBodies() {
        auto& bodyInterface = world->body_interface();
        for (const auto& record : boxes) {
            bodyInterface.RemoveBody(record.id);
            bodyInterface.DestroyBody(record.id);
        }
        boxes.clear();
        if (!floor.IsInvalid()) {
            bodyInterface.RemoveBody(floor);
            bodyInterface.DestroyBody(floor);
            floor = JPH::BodyID();
        }
    }

    bool configure(std::size_t requested) {
        const auto count = std::min(requested, MaxDynamicBodies);
        if (count == boxes.size())
            return true;
        destroyBodies();
        JPH::BoxShapeSettings shapeSettings(JPH::Vec3(.45f, .45f, .45f));
        auto shape = shapeSettings.Create();
        if (shape.HasError())
            return false;
        boxes.reserve(count);
        if (!createFloor())
            return false;
        const std::size_t stackCount = std::max<std::size_t>(1, (count + 7) / 8);
        const std::size_t width =
            std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(std::sqrt(stackCount))));
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t stack = i / 8;
            const std::size_t layer = i % 8;
            const std::size_t row = stack / width;
            const std::size_t column = stack % width;
            const float x =
                (static_cast<float>(column) - static_cast<float>(width - 1) * .5f) * 1.02f;
            const float z = (static_cast<float>(row) - static_cast<float>(width - 1) * .5f) * 1.02f;
            const float y = 1.2f + static_cast<float>(layer) * .95f;
            constexpr float halfExtent = .45f;
            const auto rotation =
                JPH::Quat::sRotation(JPH::Vec3(0, 1, 0), static_cast<float>(i % 7) * .08f);
            JPH::BodyCreationSettings settings(shape.Get(), JPH::RVec3(x, y, z), rotation,
                                               JPH::EMotionType::Dynamic, MovingLayer);
            settings.mFriction = .65f;
            settings.mRestitution = .05f;
            const auto id =
                world->body_interface().CreateAndAddBody(settings, JPH::EActivation::Activate);
            if (id.IsInvalid())
                break;
            boxes.push_back({id, meshes.cubes[i % meshes.cubes.size()], halfExtent});
        }
        accumulator = 0;
        ticks = 0;
        metricsValue = {};
        metricsValue.entities = boxes.size();
        return boxes.size() == count;
    }

    bool step(float deltaSeconds) {
        if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0)
            return false;
        const auto started = Clock::now();
        const auto helperCountBefore = executor.workerCompletedCount();
        metricsValue.helperCompletedDelta = 0;
        accumulator = std::min(accumulator + static_cast<double>(deltaSeconds), FixedStep * 8.0);
        while (accumulator >= FixedStep) {
            if (world->step(static_cast<float>(FixedStep), CollisionSteps) !=
                JPH::EPhysicsUpdateError::None)
                return false;
            accumulator -= FixedStep;
            ++ticks;
        }
        metricsValue.stepMilliseconds = elapsedMilliseconds(started);
        metricsValue.helperCompletedDelta = executor.workerCompletedCount() - helperCountBefore;
        metricsValue.ticks = ticks;
        metricsValue.entities = boxes.size();
        return true;
    }

    void extract(std::vector<engine::graphics::Draw>& output) {
        const auto started = Clock::now();
        output.clear();
        output.reserve(boxes.size() + 1);
        output.push_back(
            {meshes.cubes[0], engine::translate({0, -.5f, 0}) * engine::scale({80, 1, 80})});
        std::uint64_t checksum = 1469598103934665603ull;
        const auto& bodyInterface = world->body_interface();
        for (const auto& record : boxes) {
            JPH::RVec3 position;
            JPH::Quat rotation;
            bodyInterface.GetPositionAndRotation(record.id, position, rotation);
            const engine::V3 p{static_cast<float>(position.GetX()),
                               static_cast<float>(position.GetY()),
                               static_cast<float>(position.GetZ())};
            const auto q = rotation;
            const engine::M4 transform =
                engine::translate(p) * engine::rotation({q.GetX(), q.GetY(), q.GetZ(), q.GetW()}) *
                engine::scale(
                    {record.halfExtent * 2, record.halfExtent * 2, record.halfExtent * 2});
            output.push_back({record.mesh, transform});
            const auto* values = reinterpret_cast<const unsigned char*>(&p);
            for (std::size_t i = 0; i < sizeof(p); ++i) {
                checksum ^= values[i];
                checksum *= 1099511628211ull;
            }
        }
        metricsValue.extractionMilliseconds = elapsedMilliseconds(started);
        metricsValue.visibleDraws = output.size();
        metricsValue.checksum = checksum;
        metricsValue.entities = boxes.size();
        metricsValue.systems = 1;
    }

    engine::jobs::Executor& executor;
    MeshIds meshes;
    JPH::BroadPhaseLayerInterfaceTable layers;
    JPH::ObjectLayerPairFilterTable pairs;
    std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> broadPhaseFilter;
    std::unique_ptr<engine::physics::PhysicsWorld> world;
    JPH::BodyID floor;
    std::vector<BoxRecord> boxes;
    WorkloadMetrics metricsValue;
    double accumulator = 0;
    std::uint64_t ticks = 0;
    bool initialized = false;
};

namespace {
class PhysicsWorkload final : public Workload {
  public:
    PhysicsWorkload(engine::jobs::Executor& executor, const MeshIds& meshes, std::size_t count)
        : impl_(std::make_unique<PhysicsWorkloadImpl>(executor, meshes, count)) {}
    bool configure(std::size_t count) override {
        return impl_->configure(count);
    }
    bool step(float deltaSeconds) override {
        return impl_->step(deltaSeconds);
    }
    void extract(std::vector<engine::graphics::Draw>& output) override {
        impl_->extract(output);
    }
    WorkloadMetrics metrics() const override {
        return impl_->metricsValue;
    }
    bool initialized() const {
        return impl_->initialized;
    }

  private:
    std::unique_ptr<PhysicsWorkloadImpl> impl_;
};
} // namespace

std::unique_ptr<Workload> createPhysicsWorkload(engine::jobs::Executor& executor,
                                                const MeshIds& meshes, std::size_t count) {
    auto workload = std::make_unique<PhysicsWorkload>(executor, meshes, count);
    if (!workload->initialized())
        return {};
    return workload;
}

} // namespace engine::demo
