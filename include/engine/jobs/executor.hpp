#pragma once
#include "engine/jobs/job_scheduler.hpp"
#include <memory>

namespace engine::jobs {
struct RunResult {
    bool success = false;
    std::size_t completed = 0;
    std::string error;
};

// The executor supplies optional host workers for the portable scheduler. Empty
// workers are polled with yield; platform thread creation and joining remain host work.
class Executor {
  public:
    using Ticket = JobScheduler::Ticket;

    explicit Executor(unsigned workers = 0);
    ~Executor();
    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    // Expose the owned scheduler for APIs that build task graphs directly, such
    // as the ECS system scheduler. This reuses this executor's host workers.
    JobScheduler& scheduler();
    unsigned workerCount() const;
    Ticket submit(const TaskGraph& graph);
    JobStatus poll(const Ticket& ticket) const;
    bool helpOne();
    RunResult run(const TaskGraph& graph);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace engine::jobs
