#include "engine/particles/particle_system.hpp"
#include <cassert>
#include <cmath>
#include <limits>

using namespace engine::particles;

int main() {
    Emitter emitter;
    emitter.emissionRate = 10;
    emitter.lifetime = .2f;
    emitter.size = 1;
    ParticleSystem system(3);
    system.update(.05f, std::span(&emitter, 1));
    assert(system.instances().empty());
    system.update(.05f, std::span(&emitter, 1));
    assert(system.instances().size() == 1);
    const auto frozen = system.instances().front();
    system.update(0, std::span(&emitter, 1));
    assert(system.instances().front().age == frozen.age &&
           system.instances().front().position.x == frozen.position.x);
    const float seed = frozen.seed;
    system.update(std::numeric_limits<float>::quiet_NaN(), std::span(&emitter, 1));
    assert(system.instances().size() == 1);
    assert(system.instances().front().seed == seed);
    system.update(.5f, std::span(&emitter, 1));
    assert(system.instances().size() <= 3);
    for (const auto& particle : system.instances())
        assert(std::isfinite(particle.position.x) && std::isfinite(particle.age));

    system.clear();
    emitter.emissionRate = 0;
    system.update(1, std::span(&emitter, 1));
    emitter.emissionRate = 2;
    system.update(.25f, std::span(&emitter, 1));
    assert(system.instances().empty());
    system.update(.25f, std::span(&emitter, 1));
    assert(system.instances().size() == 1);

    Emitter left = emitter, right = emitter;
    left.position = {-3, 0, 0};
    right.position = {3, 0, 0};
    left.emissionRate = right.emissionRate = 8;
    ParticleSystem twoSources(2);
    const Emitter sources[] = {left, right};
    twoSources.update(.125f, sources);
    assert(twoSources.instances().size() == 2);
    assert(twoSources.instances()[0].position.x < 0 && twoSources.instances()[1].position.x > 0);

    ParticleSystem bounded(1);
    Emitter fast = emitter;
    fast.lifetime = 1;
    fast.emissionRate = std::numeric_limits<float>::max();
    bounded.update(1, std::span(&fast, 1));
    assert(bounded.instances().size() == 1);
    fast.emissionRate = 0;
    bounded.update(3, std::span(&fast, 1));
    assert(bounded.instances().empty());
    fast.emissionRate = 2;
    bounded.update(.25f, std::span(&fast, 1));
    assert(bounded.instances().empty());
    bounded.update(.25f, std::span(&fast, 1));
    assert(bounded.instances().size() == 1);
}
