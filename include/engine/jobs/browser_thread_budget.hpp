#pragma once

namespace engine::jobs {
struct BrowserThreadBudget {
    unsigned hardwareThreads;
    unsigned computeWorkers;
};

// The browser main thread is responsive UI; render and simulation each own one
// additional thread. A missing or zero browser report uses the conservative 8.
constexpr BrowserThreadBudget browserThreadBudget(unsigned reportedThreads) {
    const unsigned hardwareThreads = reportedThreads == 0 ? 8 : reportedThreads;
    return {hardwareThreads, hardwareThreads > 2 ? hardwareThreads - 2 : 0};
}
} // namespace engine::jobs
