#include "engine/jobs/executor.hpp"

#include <atomic>
#include <thread>
#include <utility>
#include <vector>

#if defined(__EMSCRIPTEN__)
#include "engine/platform/web/host.hpp"
#endif
#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
#include <emscripten/threading.h>
#endif

#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
#define ENGINE_WORKER_THREADS 1
#else
#define ENGINE_WORKER_THREADS 0
#endif

namespace engine::jobs {
static_assert(std::atomic<std::size_t>::is_always_lock_free,
              "executor helper diagnostics must be lock-free");
namespace {
thread_local const void* activeExecutorHelper = nullptr;

void idleWorker() {
#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
    // Timer-based pacing keeps idle browser helpers from monopolizing CPU time.
    emscripten_thread_sleep(1);
#else
    std::this_thread::yield();
#endif
}

unsigned defaultWorkerCount() {
#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
    return platform::web::computeWorkerCount();
#else
    return 0;
#endif
}
} // namespace

struct Executor::Impl {
    JobScheduler scheduler;
    unsigned requestedWorkers = 0;
    std::atomic<bool> stopping{false};
    std::atomic<std::size_t> workerCompleted{0};
#if ENGINE_WORKER_THREADS
    std::vector<std::thread> workers;
#endif

    explicit Impl(unsigned count) : requestedWorkers(count) {
#if ENGINE_WORKER_THREADS
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
        try {
            workers.reserve(count);
        } catch (...) {
            return;
        }
#else
        workers.reserve(count);
#endif
        for (unsigned i = 0; i < count; ++i) {
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
            try {
#endif
                workers.emplace_back([this] {
                    activeExecutorHelper = this;
                    while (!stopping.load(std::memory_order_acquire)) {
                        if (scheduler.helpOne())
                            workerCompleted.fetch_add(1, std::memory_order_relaxed);
                        else
                            idleWorker();
                    }
                });
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
            } catch (...) {
                break;
            }
#endif
        }
#else
        (void)count;
#endif
    }

    ~Impl() {
        // Drain accepted work before stopping host workers. Destruction may execute
        // remaining tasks on this thread, and may therefore take as long as task work.
        while (scheduler.hasOutstanding()) {
            if (!scheduler.helpOne())
                idleWorker();
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

    bool startupFailed() const {
        return count() != requestedWorkers;
    }
};

Executor::Executor() : Executor(defaultWorkerCount()) {}
Executor::Executor(unsigned workers) : impl_(std::make_unique<Impl>(workers)) {}
Executor::~Executor() = default;
JobScheduler& Executor::scheduler() {
    return impl_->scheduler;
}
unsigned Executor::requestedWorkerCount() const {
    return impl_->requestedWorkers;
}
unsigned Executor::workerCount() const {
    return impl_->count();
}
bool Executor::workerStartupFailed() const {
    return impl_->startupFailed();
}
std::size_t Executor::workerCompletedCount() const {
    return impl_->workerCompleted.load(std::memory_order_relaxed);
}
bool Executor::isHelperThread() const {
    return activeExecutorHelper == impl_.get();
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
            idleWorker();
    }
}
} // namespace engine::jobs
