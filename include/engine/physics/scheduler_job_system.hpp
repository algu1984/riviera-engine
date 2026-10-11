#pragma once

#include "engine/jobs/job_scheduler.hpp"

#include <Jolt/Jolt.h>

#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystem.h>

#include <atomic>

namespace engine::physics {

// Adapts Jolt's dependency jobs to the Engine's shared scheduler. The scheduler
// and any host workers must outlive this object. Jolt jobs and barriers must be
// finished and destroyed before this adapter; Jolt job callbacks must not throw.
class SchedulerJobSystem final : public JPH::JobSystem {
  public:
    SchedulerJobSystem(engine::jobs::JobScheduler& scheduler, JPH::uint maxJobs,
                       int maxConcurrency);
    ~SchedulerJobSystem() override;

    int GetMaxConcurrency() const override;
    JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function,
                        JPH::uint32 dependencies = 0) override;
    Barrier* CreateBarrier() override;
    void DestroyBarrier(Barrier* barrier) override;
    void WaitForJobs(Barrier* barrier) override;

  protected:
    void QueueJob(Job* job) override;
    void QueueJobs(Job** jobs, JPH::uint count) override;
    void FreeJob(Job* job) override;
    void ExecuteJob(Job* job);

  private:
    class BarrierImpl final : public Barrier {
      public:
        void AddJob(const JobHandle& job) override;
        void AddJobs(const JobHandle* jobs, JPH::uint count) override;

      protected:
        void OnJobFinished(Job* job) override;

      private:
        friend class SchedulerJobSystem;
        std::atomic<JPH::uint32> pending_{0};
    };

    using AvailableJobs = JPH::FixedSizeFreeList<Job>;

    engine::jobs::JobScheduler& scheduler_;
    AvailableJobs jobs_;
    int maxConcurrency_;
    std::atomic<JPH::uint32> callbacksInFlight_{0};
};

} // namespace engine::physics
