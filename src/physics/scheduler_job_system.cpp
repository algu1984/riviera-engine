#include "engine/physics/scheduler_job_system.hpp"

#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Physics/PhysicsSettings.h>

#include <algorithm>
#include <exception>
#include <limits>
#include <thread>
#include <utility>

namespace engine::physics {
namespace {
constexpr JPH::uint kJobPageSize = 64;
static_assert(std::atomic<JPH::uint32>::is_always_lock_free,
              "Jolt adapter counters must use lock-free atomics");

JPH::uint normalized_max_jobs(JPH::uint maxJobs) {
    return maxJobs == 0 ? JPH::cMaxPhysicsJobs : maxJobs;
}
} // namespace

SchedulerJobSystem::SchedulerJobSystem(engine::jobs::JobScheduler& scheduler, JPH::uint maxJobs,
                                       int maxConcurrency)
    : scheduler_(scheduler),
      maxConcurrency_(
          std::max(1, std::min(maxConcurrency,
                               static_cast<int>(std::min<JPH::uint>(
                                   normalized_max_jobs(maxJobs),
                                   static_cast<JPH::uint>(std::numeric_limits<int>::max())))))) {
    maxJobs = normalized_max_jobs(maxJobs);
    auto pageSize = kJobPageSize;
    while (maxJobs % pageSize != 0)
        pageSize /= 2;
    jobs_.Init(maxJobs, pageSize);
}

int SchedulerJobSystem::GetMaxConcurrency() const {
    return maxConcurrency_;
}

SchedulerJobSystem::~SchedulerJobSystem() {
    while (callbacksInFlight_.load(std::memory_order_acquire) != 0) {
        if (!scheduler_.helpOne())
            std::this_thread::yield();
    }
}

JPH::JobSystem::JobHandle SchedulerJobSystem::CreateJob(const char* name, JPH::ColorArg color,
                                                        const JobFunction& function,
                                                        JPH::uint32 dependencies) {
    const JPH::uint32 index = jobs_.ConstructObject(name, color, this, function, dependencies);
    if (index == AvailableJobs::cInvalidObjectIndex) {
        JPH_ASSERT(false, "Jolt job capacity exhausted");
        return {};
    }

    Job* job = &jobs_.Get(index);
    JobHandle handle(job);
    if (dependencies == 0)
        QueueJob(job);
    return handle;
}

JPH::JobSystem::Barrier* SchedulerJobSystem::CreateBarrier() {
    return new BarrierImpl();
}

void SchedulerJobSystem::DestroyBarrier(Barrier* barrier) {
    auto* implementation = static_cast<BarrierImpl*>(barrier);
    JPH_ASSERT(implementation->pending_.load(std::memory_order_acquire) == 0);
    delete implementation;
}

void SchedulerJobSystem::WaitForJobs(Barrier* barrier) {
    auto* implementation = static_cast<BarrierImpl*>(barrier);
    while (implementation->pending_.load(std::memory_order_acquire) != 0) {
        if (!scheduler_.helpOne())
            std::this_thread::yield();
    }

    // Jolt calls OnJobFinished before its Engine task has returned and released
    // its queue reference. Keep the world alive until those callbacks unwind.
    while (callbacksInFlight_.load(std::memory_order_acquire) != 0) {
        if (!scheduler_.helpOne())
            std::this_thread::yield();
    }
}

void SchedulerJobSystem::QueueJob(Job* job) {
    job->AddRef();
    callbacksInFlight_.fetch_add(1, std::memory_order_release);
    engine::jobs::JobScheduler::Ticket ticket;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
    try {
#endif
        engine::jobs::TaskGraph graph;
        graph.add("Jolt physics job", [this, job] {
            ExecuteJob(job);
            return true;
        });
        ticket = scheduler_.submit(graph);
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
    } catch (...) {
        // The scheduler did not accept this callback. Run it on the current
        // Jolt thread so its dependency graph can still make progress.
        ExecuteJob(job);
        return;
    }
#endif
    const auto status = scheduler_.poll(ticket);
    if (status.complete && !status.success) {
        // JobScheduler reports queue-admission failure through a completed
        // failed ticket instead of throwing. Preserve Jolt progress inline.
        ExecuteJob(job);
    }
}

void SchedulerJobSystem::ExecuteJob(Job* job) {
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
    try {
        job->Execute();
    } catch (...) {
        job->Release();
        callbacksInFlight_.fetch_sub(1, std::memory_order_acq_rel);
        std::terminate();
    }
#else
    job->Execute();
#endif
    job->Release();
    callbacksInFlight_.fetch_sub(1, std::memory_order_acq_rel);
}

void SchedulerJobSystem::QueueJobs(Job** jobs, JPH::uint count) {
    for (JPH::uint index = 0; index < count; ++index)
        QueueJob(jobs[index]);
}

void SchedulerJobSystem::FreeJob(Job* job) {
    jobs_.DestructObject(job);
}

void SchedulerJobSystem::BarrierImpl::AddJob(const JobHandle& job) {
    if (!job.IsValid())
        return;

    // Count before publishing the barrier pointer. A worker may finish the job
    // immediately after SetBarrier succeeds and invoke OnJobFinished at once.
    pending_.fetch_add(1, std::memory_order_acq_rel);
    if (!job.GetPtr()->SetBarrier(this))
        pending_.fetch_sub(1, std::memory_order_acq_rel);
}

void SchedulerJobSystem::BarrierImpl::AddJobs(const JobHandle* jobs, JPH::uint count) {
    for (JPH::uint index = 0; index < count; ++index)
        AddJob(jobs[index]);
}

void SchedulerJobSystem::BarrierImpl::OnJobFinished(Job*) {
    pending_.fetch_sub(1, std::memory_order_acq_rel);
}

} // namespace engine::physics
