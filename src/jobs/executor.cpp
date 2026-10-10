#include "engine/jobs/executor.hpp"

#include <atomic>
#include <thread>
#include <utility>
#include <vector>

#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
#define ENGINE_WORKER_THREADS 1
#else
#define ENGINE_WORKER_THREADS 0
#endif

namespace engine::jobs {
struct Executor::Impl {
    JobScheduler scheduler;
    std::atomic<bool> stopping{false};
#if ENGINE_WORKER_THREADS
    std::vector<std::thread> workers;
#endif

    explicit Impl(unsigned count) {
#if ENGINE_WORKER_THREADS
        workers.reserve(count);
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
        try {
#endif
            for (unsigned i = 0; i < count; ++i) {
                workers.emplace_back([this] {
                    while (!stopping.load(std::memory_order_acquire)) {
                        if (!scheduler.helpOne())
                            std::this_thread::yield();
                    }
                });
            }
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
        } catch (...) {
            stopping.store(true, std::memory_order_release);
            for (auto& worker : workers)
                if (worker.joinable())
                    worker.join();
            throw;
        }
#endif
#else
        (void)count;
#endif
    }

    ~Impl() {
        // Drain accepted work before stopping host workers. Destruction may execute
        // remaining tasks on this thread, and may therefore take as long as task work.
        while (scheduler.hasOutstanding()) {
            if (!scheduler.helpOne())
                std::this_thread::yield();
        }
        stopping.store(true, std::memory_order_release);
#if ENGINE_WORKER_THREADS
        for (auto& worker : workers)
            worker.join();
#endif
    }

    unsigned count() const {
#if ENGINE_WORKER_THREADS
        return static_cast<unsigned>(workers.size());
#else
        return 0;
#endif
    }
};

Executor::Executor(unsigned workers) : impl_(std::make_unique<Impl>(workers)) {}
Executor::~Executor() = default;
JobScheduler& Executor::scheduler() {
    return impl_->scheduler;
}
unsigned Executor::workerCount() const {
    return impl_->count();
}
Executor::Ticket Executor::submit(const TaskGraph& graph) {
    return impl_->scheduler.submit(graph);
}
JobStatus Executor::poll(const Ticket& ticket) const {
    return impl_->scheduler.poll(ticket);
}
bool Executor::helpOne() {
    return impl_->scheduler.helpOne();
}
RunResult Executor::run(const TaskGraph& graph) {
    if (JobScheduler::inTask())
        return {false, 0, "A task cannot synchronously wait on an executor"};
    auto ticket = submit(graph);
    for (;;) {
        const auto status = poll(ticket);
        if (status.complete)
            return {status.success, status.completed, status.error};
        if (!helpOne())
            std::this_thread::yield();
    }
}
} // namespace engine::jobs
