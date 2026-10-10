#include "engine/graphics/frame_graph/frame_graph.hpp"
#include "engine/jobs/executor.hpp"
#include "engine/math.hpp"
#include "engine/runtime/fixed_step_clock.hpp"
#include "engine/runtime/snapshot_exchange.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

static void require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", message);
        std::exit(1);
    }
}
static void tasks() {
    using namespace engine::jobs;
    {
        Executor cooperative;
        std::atomic<int> state{0};
        TaskGraph graph;
        const auto first = graph.add("async first", [&] {
            state.store(1, std::memory_order_relaxed);
            return true;
        });
        graph.add("async second",
                  [&] {
                      state.store(2, std::memory_order_relaxed);
                      return true;
                  },
                  {first});
        auto ticket = cooperative.submit(graph);
        require(!cooperative.poll(ticket).complete, "submit is nonblocking");
        require(cooperative.helpOne(), "caller can help ready work");
        require(!cooperative.poll(ticket).complete && state.load() == 1,
                "poll reports active dependent graph");
        require(cooperative.helpOne(), "help executes unblocked dependent");
        const auto done = cooperative.poll(ticket);
        require(done.complete && done.success && done.completed == 2 && state.load() == 2,
                "executor async ticket completes");
    }
    {
        Executor pool(3);
        constexpr int producerCount = 4;
        constexpr int graphsPerProducer = 24;
        std::atomic<int> calls{0};
        std::vector<std::vector<Executor::Ticket>> tickets(producerCount);
        std::vector<std::thread> producers;
        for (int producer = 0; producer < producerCount; ++producer) {
            producers.emplace_back([&, producer] {
                auto& ownedTickets = tickets[producer];
                ownedTickets.reserve(graphsPerProducer);
                for (int i = 0; i < graphsPerProducer; ++i) {
                    TaskGraph graph;
                    graph.add("concurrent submit", [&calls] {
                        calls.fetch_add(1, std::memory_order_relaxed);
                        return true;
                    });
                    ownedTickets.push_back(pool.submit(graph));
                }
            });
        }
        for (auto& producer : producers)
            producer.join();
        for (const auto& group : tickets)
            for (const auto& ticket : group) {
                auto result = pool.poll(ticket);
                while (!result.complete) {
                    if (!pool.helpOne())
                        std::this_thread::yield();
                    result = pool.poll(ticket);
                }
                require(result.success && result.completed == 1,
                        "executor concurrent submission ticket completes");
            }
        require(calls.load(std::memory_order_relaxed) == producerCount * graphsPerProducer,
                "executor runs each concurrently submitted graph once");
    }
    for (unsigned workers : {0u, 1u, 3u}) {
        Executor pool(workers);
        TaskGraph graph;
        std::atomic<int> state{0};
        auto a = graph.add("input", [&] {
            state = 1;
            return true;
        });
        auto b = graph.add("scene",
                           [&] {
                               require(state.load() & 1, "scene waits input");
                               state.fetch_or(2);
                               return true;
                           },
                           {a});
        auto c = graph.add("UI",
                           [&] {
                               require(state.load() & 1, "UI waits input");
                               state.fetch_or(4);
                               return true;
                           },
                           {a});
        graph.add("submit", [&] { return state.load() == 7; }, {b, c});
        auto result = pool.run(graph);
        require(result.success && result.completed == 4, "serial/pool DAG completes");
        for (int i = 0; i < 20; ++i)
            require(pool.run(graph).success, "persistent pool reused");
        TaskGraph failed;
        auto f = failed.add("failure", [] { return false; });
        bool ran = false;
        failed.add("cancelled dependent",
                   [&] {
                       ran = true;
                       return true;
                   },
                   {f});
        require(!pool.run(failed).success && !ran, "failure cancels dependents");
        TaskGraph cycle;
        auto x = cycle.add("x", [] { return true; });
        auto y = cycle.add("y", [] { return true; }, {x});
        cycle.depends(x, y);
        require(!pool.run(cycle).success, "task cycle rejected");
        TaskGraph invalid;
        invalid.add("invalid", [] { return true; }, {99});
        require(!pool.run(invalid).success, "invalid dependency rejected");
        TaskGraph nested, outer;
        nested.add("inner", [] { return true; });
        outer.add("outer", [&] { return !pool.run(nested).success; });
        require(pool.run(outer).success, "same executor reentrancy rejected without deadlock");
        Executor other(1);
        TaskGraph crossPool;
        crossPool.add("cross-pool wait", [&] { return !other.run(nested).success; });
        require(pool.run(crossPool).success, "cross-pool synchronous waits rejected");
    }
    Executor pool(2);
    TaskGraph concurrent;
    std::atomic<int> entered{0};
    auto both = [&] {
        entered.fetch_add(1, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (entered.load(std::memory_order_acquire) != 2 &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        return entered.load(std::memory_order_acquire) == 2;
    };
    concurrent.add("parallel A", both);
    concurrent.add("parallel B", both);
    require(pool.run(concurrent).success && entered.load(std::memory_order_relaxed) == 2,
            "independent tasks actually use two workers");
}
static void clocks() {
    using namespace engine::runtime;
    FixedStepClock clock;
    clock.beginFrame(.015);
    require(clock.consumeStep() && !clock.consumeStep(), "15ms produces one 120Hz tick");
    require(std::abs(clock.alpha() - .8) < 1e-6, "15ms interpolates leftover time");
    clock.beginFrame(1. / 120.);
    require(clock.consumeStep() && !clock.consumeStep(), "fixed next tick");
    require(std::abs(clock.alpha() - .8) < 1e-6,
            "interpolation stable across variable frame intervals");
    clock.reset();
    clock.beginFrame(1.);
    int ticks = 0;
    while (clock.consumeStep())
        ++ticks;
    require(ticks == 12 && clock.discardedSeconds() > .89,
            "suspension limits catch-up without large physics dt");
    clock.beginFrame(.006);
    clock.beginFrame(.006, false);
    require(clock.alpha() == 0 && !clock.consumeStep(), "pause discards residual interpolation");
    clock.reset();
    require(clock.tick() == 0 && clock.alpha() == 0, "reset starts new timeline");
    SnapshotExchange<int> exchange;
    require(exchange.reset(10), "snapshot initial seed");
    auto seed = exchange.acquire();
    require(exchange.publish(11, 1, 1. / 120.), "first tick published");
    auto first = exchange.acquire();
    require(exchange.publish(12, 2, 2. / 120.), "second tick published");
    auto second = exchange.acquire();
    require(std::abs(second.alpha(1.5 / 120.) - .5) < 1e-6, "snapshot timestamps interpolate");
    require(second.alpha(-1) == 0 && second.alpha(10) == 1, "presentation holds history edges");
    require(!exchange.publish(13, 3, 3. / 120.), "pinned readers bound publication memory");
    require(!exchange.reset(100, 0, 0), "pinned readers can reject reset");
    require(seed.current->value == 10 && first.current->value == 11, "old readers stay immutable");
    seed = {};
    first = {};
    require(exchange.reset(100, 0, 0), "reset retries after readers release");
    auto reset = exchange.acquire();
    require(reset.current->discontinuity && reset.previous == reset.current &&
                reset.alpha(-10) == 1,
            "teleport seeds both snapshots");
    require(second.current->value == 12, "reader survives reset");
}
static engine::graphics::ResourceDesc color() {
    using namespace engine::graphics;
    return {Kind::Texture,
            "rgba8unorm",
            640,
            480,
            1,
            mask(Usage::ColorAttachment) | mask(Usage::Sampled)};
}
static void graphs() {
    using namespace engine::graphics;
    FrameGraph graph;
    auto first = graph.resource("first", color()), second = graph.resource("second", color());
    auto surface = graph.resource("surface", color(), true);
    auto a = graph.next(first), b = graph.next(second), out = graph.next(surface);
    int stage = 0;
    // Intentionally declare consumer before producer: dependency order wins.
    graph.pass("read first", {Access::read(a, Usage::Sampled)}, [&] {
        require(stage == 1, "graph RAW order");
        stage = 2;
    });
    graph.pass("write first", {Access::write(a, Usage::ColorAttachment)}, [&] {
        require(stage == 0, "graph producer first");
        stage = 1;
    });
    graph.pass("write second",
               {Access::write(b, Usage::ColorAttachment), Access::read(out, Usage::Sampled)}, [&] {
                   require(stage == 3, "reuse follows reader lifetime");
                   stage = 4;
               });
    graph.pass("surface",
               {Access::write(out, Usage::ColorAttachment), Access::read(a, Usage::Sampled)}, [&] {
                   require(stage == 2, "surface waits first");
                   stage = 3;
               });
    require(graph.execute() && stage == 4, "graph executes sorted callbacks");
    std::uint32_t slotA = FrameGraph::noSlot, slotB = FrameGraph::noSlot;
    for (auto& l : graph.lifetimes()) {
        if (l.resource == first)
            slotA = l.slot;
        if (l.resource == second)
            slotB = l.slot;
        if (l.resource == surface)
            require(l.slot == FrameGraph::noSlot, "imported surface excluded from pool");
    }
    require(slotA == slotB && graph.slots().size() == 1,
            "nonoverlapping compatible textures reuse slot");
    FrameGraph bad;
    auto r = bad.resource("uninitialized", color());
    bad.pass("read", {Access::read(bad.initial(r), Usage::Sampled)}, {});
    require(!bad.compile(), "transient read-before-write rejected");
    FrameGraph ambiguous;
    auto q = ambiguous.resource("ambiguous", color());
    auto v = ambiguous.next(q);
    ambiguous.pass("write A", {Access::write(v, Usage::ColorAttachment)}, {});
    ambiguous.pass("write B", {Access::write(v, Usage::ColorAttachment)}, {});
    require(!ambiguous.compile(), "ambiguous version writer rejected");
    FrameGraph cycle;
    auto x = cycle.resource("x", color()), y = cycle.resource("y", color());
    auto vx = cycle.next(x), vy = cycle.next(y);
    cycle.pass("x", {Access::read(vy, Usage::Sampled), Access::write(vx, Usage::ColorAttachment)},
               {});
    cycle.pass("y", {Access::read(vx, Usage::Sampled), Access::write(vy, Usage::ColorAttachment)},
               {});
    require(!cycle.compile(), "resource dependency cycle rejected");
    FrameGraph incompatible;
    auto d = color();
    d.samples = 4;
    auto ms = incompatible.resource("MSAA", d);
    d.format = "depth32float";
    d.samples = 1;
    d.usage = mask(Usage::DepthAttachment);
    auto depth = incompatible.resource("depth", d);
    incompatible.pass("mismatch",
                      {Access::write(incompatible.next(ms), Usage::ColorAttachment),
                       Access::write(incompatible.next(depth), Usage::DepthAttachment)},
                      {});
    require(!incompatible.compile(), "depth/color sample mismatch rejected");
    FrameGraph feedback;
    auto f = feedback.resource("feedback", color());
    auto f1 = feedback.next(f);
    feedback.pass("feedback",
                  {Access::write(f1, Usage::ColorAttachment), Access::read(f1, Usage::Sampled)},
                  {});
    require(!feedback.compile(), "sampled attachment feedback rejected");
    FrameGraph discard;
    auto dr = discard.resource("discarded", color());
    auto dv = discard.next(dr);
    discard.pass("discard", {Access::write(dv, Usage::ColorAttachment, false)}, {});
    discard.pass("read discarded", {Access::read(dv, Usage::Sampled)}, {});
    require(!discard.compile(), "read after StoreOp discard rejected");
    FrameGraph resolve;
    auto md = color();
    md.samples = 4;
    md.usage |= mask(Usage::ResolveSource);
    auto mr = resolve.resource("MSAA", md);
    auto sd = color();
    sd.usage |= mask(Usage::ResolveTarget) | mask(Usage::Present);
    auto sr = resolve.resource("surface", sd, true);
    auto mv = resolve.next(mr), sv = resolve.next(sr);
    resolve.pass("resolve",
                 {Access::write(mv, Usage::ColorAttachment, false),
                  Access::read(mv, Usage::ResolveSource), Access::write(sv, Usage::ResolveTarget)},
                 {});
    resolve.pass("present", {Access::read(sv, Usage::Present)}, {});
    require(resolve.compile(), "same-pass resolve permits discarded MSAA source");
    FrameGraph cull;
    auto unused = cull.resource("unused", color());
    bool executed = false;
    cull.pass(
        "unused", {Access::write(cull.next(unused), Usage::ColorAttachment)},
        [&] { executed = true; }, false);
    cull.pass("present", {}, [] {}, true);
    require(cull.execute() && !executed, "unrooted pass pruned");
    // A newer write waits for readers of the prior physical texture version.
    FrameGraph versions;
    auto vr = versions.resource("history", color(), true);
    auto v1 = versions.next(vr), v2 = versions.next(vr);
    int order = 0;
    versions.pass("overwrite", {Access::write(v2, Usage::ColorAttachment)}, [&] {
        require(order == 2, "WAR ordering");
        order = 3;
    });
    versions.pass("read old", {Access::read(v1, Usage::Sampled)}, [&] {
        require(order == 1, "read version after producer");
        order = 2;
    });
    versions.pass("producer", {Access::write(v1, Usage::ColorAttachment)}, [&] { order = 1; });
    require(versions.execute() && order == 3, "versioned resource overwrite safe");
}
static void rotations() {
    using namespace engine;
    for (auto end : {rotateX(pi), rotateY(pi), rotateZ(pi), rotateY(1.5f) * rotateX(2.1f)}) {
        auto mid = rotation(slerp(quaternion(M4::identity()), quaternion(end), .5f));
        auto x = direction(mid, {1, 0, 0}), y = direction(mid, {0, 1, 0}),
             z = direction(mid, {0, 0, 1});
        require(std::abs(length(x) - 1) < 1e-5 && std::abs(length(y) - 1) < 1e-5 &&
                    std::abs(length(z) - 1) < 1e-5,
                "180-degree interpolation remains orthonormal");
        require(std::abs(dot(x, y)) < 1e-5 && dot(cross(x, y), z) > .9999f,
                "quaternion orientation preserves handedness");
        auto restored = rotation(quaternion(end));
        for (int i = 0; i < 16; ++i)
            require(std::abs(restored.m[i] - end.m[i]) < 1e-5, "matrix quaternion round trip");
    }
    auto near = rotation(
        slerp(quaternion(rotateY(179 * pi / 180)), quaternion(rotateY(-179 * pi / 180)), .5f));
    require(direction(near, {0, 0, 1}).z < -.999f, "orientation follows shortest arc across wrap");
}
int main() {
    tasks();
    clocks();
    graphs();
    rotations();
    std::puts("PASS engine DAG workers/failures, fixed clock/snapshot lifetime, frame graph "
              "versions/hazards/pooling, quaternion interpolation");
}
