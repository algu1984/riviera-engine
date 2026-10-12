#pragma once

#include "workload.hpp"

namespace engine::demo {

std::unique_ptr<Workload> createJobsWorkload(engine::jobs::Executor& executor,
                                             const MeshIds& meshes, std::size_t count);

} // namespace engine::demo
