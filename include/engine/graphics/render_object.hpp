#pragma once

#include <engine/math.hpp>

namespace engine::graphics {

struct Draw {
    int mesh = -1;
    engine::M4 transform = engine::M4::identity();
};

} // namespace engine::graphics
