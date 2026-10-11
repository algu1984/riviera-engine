#include "engine/particles/particle_system.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace engine::particles {
ParticleSystem::ParticleSystem(std::size_t capacity) : maximumParticles_(capacity) {
    particles_.reserve(capacity);
    visible_.reserve(capacity);
}

float ParticleSystem::randomUnit() {
    randomState_ ^= randomState_ << 13;
    randomState_ ^= randomState_ >> 17;
    randomState_ ^= randomState_ << 5;
    return float(randomState_ >> 8) * (1.f / 16777216.f);
}

void ParticleSystem::clear() {
    particles_.clear();
    visible_.clear();
    emissionRemainders_.clear();
    previousPositions_.clear();
    hasPreviousPosition_.clear();
}

void ParticleSystem::update(float dt, std::span<const Emitter> emitters) {
    if (!std::isfinite(dt) || dt <= 0)
        return;
    dt = std::min(dt, 1.f);
    if (emissionRemainders_.size() != emitters.size())
        emissionRemainders_.resize(emitters.size(), 0);
    if (previousPositions_.size() != emitters.size()) {
        previousPositions_.resize(emitters.size());
        hasPreviousPosition_.resize(emitters.size(), false);
    }

    for (std::size_t i = 0; i < particles_.size();) {
        auto& particle = particles_[i];
        particle.instance.age += dt;
        if (particle.instance.age >= particle.instance.lifetime) {
            particles_[i] = particles_.back();
            particles_.pop_back();
            continue;
        }
        particle.instance.position =
            particle.instance.position + particle.velocity * dt + V3{0, .175f * dt * dt, 0};
        particle.velocity.y += .35f * dt;
        ++i;
    }

    for (std::size_t emitterIndex = 0; emitterIndex < emitters.size(); ++emitterIndex) {
        const auto& emitter = emitters[emitterIndex];
        if (!std::isfinite(emitter.emissionRate) || emitter.emissionRate <= 0 ||
            !std::isfinite(emitter.lifetime) || emitter.lifetime <= 0 ||
            !std::isfinite(emitter.size) || emitter.size <= 0) {
            emissionRemainders_[emitterIndex] = 0;
            hasPreviousPosition_[emitterIndex] = false;
            continue;
        }
        const V3 start = hasPreviousPosition_[emitterIndex] ? previousPositions_[emitterIndex]
                                                            : emitter.position;
        previousPositions_[emitterIndex] = emitter.position;
        hasPreviousPosition_[emitterIndex] = true;
        const double amount =
            emissionRemainders_[emitterIndex] +
            std::min(double(emitter.emissionRate) * dt, double(maximumParticles_) + 1.0);
        const double whole = std::floor(amount);
        const auto available = maximumParticles_ - particles_.size();
        const auto count = std::size_t(std::min(whole, double(available)));
        emissionRemainders_[emitterIndex] = whole > double(available) ? 0 : amount - whole;
        for (std::size_t n = 0; n < count; ++n) {
            V3 randomDirection{randomUnit() * 2 - 1, randomUnit() * 2 - 1, randomUnit() * 2 - 1};
            randomDirection = norm(randomDirection);
            V3 velocity = emitter.initialVelocity + randomDirection * emitter.spread;
            const float fraction = (float(n) + .5f) / float(count);
            const float age = dt * (1 - fraction);
            if (age >= emitter.lifetime)
                continue;
            const V3 position = start + (emitter.position - start) * fraction;
            const V3 spawnPosition = position + velocity * age + V3{0, .175f * age * age, 0};
            velocity.y += .35f * age;
            particles_.push_back({{spawnPosition, emitter.color, emitter.size, emitter.opacity, age,
                                   emitter.lifetime, randomUnit() * 1000.f},
                                  velocity});
        }
    }

    visible_.clear();
    for (const auto& particle : particles_)
        visible_.push_back(particle.instance);
}
} // namespace engine::particles
