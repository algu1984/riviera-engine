#pragma once
#include <cstddef>
#include <cstdint>
#include <engine/math.hpp>
#include <span>
#include <vector>

namespace engine::particles {
struct Emitter {
    V3 position{};
    V3 initialVelocity{};
    V3 color{1, 1, 1};
    float emissionRate = 0;
    float lifetime = 1;
    float size = 1;
    float opacity = 1;
    float spread = 0;
};

struct ParticleInstance {
    V3 position{};
    V3 color{1, 1, 1};
    float size = 1;
    float opacity = 1;
    float age = 0;
    float lifetime = 1;
    float seed = 0;
};

class ParticleSystem {
  public:
    explicit ParticleSystem(std::size_t capacity = 4096);
    void update(float dt, std::span<const Emitter> emitters);
    void clear();
    const std::vector<ParticleInstance>& instances() const {
        return visible_;
    }

  private:
    struct Particle {
        ParticleInstance instance;
        V3 velocity{};
    };
    std::vector<Particle> particles_;
    std::vector<ParticleInstance> visible_;
    std::vector<double> emissionRemainders_;
    std::vector<V3> previousPositions_;
    std::vector<bool> hasPreviousPosition_;
    std::size_t maximumParticles_ = 0;
    uint32_t randomState_ = 0x9e3779b9u;
    float randomUnit();
};
} // namespace engine::particles
