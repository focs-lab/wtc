// Tests of the wait whose loop is the caller's (runtime/waiter.hpp:
// begin, step, end, notify, notify_all), as a lifted wait uses it
// (shim/lifted.hpp, wtc::lifted_wait): the predicate is the program's own,
// here a pop from a std::deque under a mutex of its own, as CTPL's queue
// class is, or a stop flag, as CTPL's pool has, on every wait policy.
//
//   waiter_loop_test interleave   the lost wake-up's window, forced
//   waiter_loop_test stress [N]   N tasks, producers pausing at random, then stop
//   waiter_loop_test stop         every waiter asleep, then the stop flag
//   waiter_loop_test idle         a waiter that yields after its pause parks
//                                 once its yield budget is spent
//
// A lost wake-up leaves a waiter asleep with its predicate true and no
// notify to come; each mode waits for its waiters with a deadline and, past
// it, says "lost wake-up" and exits. With WTC_TEST_DROP_WAKEUPS the waiter
// wakes nobody, and interleave must report it.

#include "../waiter.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(__SANITIZE_THREAD__)
#define WTC_TEST_SLOW 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define WTC_TEST_SLOW 1
#endif
#endif

namespace {

using clk = std::chrono::steady_clock;
using wtc::rt::wait_policy;
using wtc::rt::waiter;

#ifdef WTC_TEST_SLOW
constexpr auto kDeadline = std::chrono::seconds(60);
#else
constexpr auto kDeadline = std::chrono::seconds(20);
#endif

const char *name(wait_policy p) {
    switch (p) {
    case wait_policy::spin: return "spin";
    case wait_policy::park: return "park";
    case wait_policy::spin_then_park: return "spin_then_park";
    }
    return "?";
}

// CTPL's detail::Queue, reduced: each operation under its own mutex.
struct locked_queue {
    std::deque<std::uint64_t> q;
    std::mutex m;
    void push(std::uint64_t v) {
        std::lock_guard<std::mutex> l(m);
        q.push_back(v);
    }
    bool pop(std::uint64_t &v) {
        std::lock_guard<std::mutex> l(m);
        if (q.empty()) return false;
        v = q.front();
        q.pop_front();
        return true;
    }
};

// CTPL's pool, reduced: the wait for "a task, or stop".
struct pool {
    locked_queue q;
    std::atomic<bool> done{false};
    waiter ev;
    explicit pool(wait_policy p, std::uint64_t spin_ns = waiter::kSpinNs, std::uint64_t yield_ns = 0)
        : ev(p, spin_ns, yield_ns) {}

    // false once stopped and empty, as CTPL's worker returns.
    bool take(std::uint64_t &v) {
        bool got = false;
        waiter::state st;
        ev.begin(st);
        while (!((got = q.pop(v)) || done.load())) ev.step(st);
        ev.end(st);
        return got;
    }
    void submit(std::uint64_t v) {
        q.push(v);
        ev.notify(1);
    }
    void stop() {
        done.store(true);
        ev.notify_all();
    }
};

[[noreturn]] void lost(const std::string &what) {
    std::printf("lost wake-up: %s\n", what.c_str());
    std::fflush(stdout);
    std::_Exit(1);
}

template <class F> void await(F &&finished, const std::string &what) {
    const auto until = clk::now() + kDeadline;
    while (!finished()) {
        if (clk::now() > until) lost(what);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// The waiter is held between its announcement and its sleep while the
// producer publishes: with no spin budget it announces at once, and the
// predicate's look is what the producer races.
int interleave() {
    for (int round = 0; round < 200; ++round) {
        pool p(wait_policy::park);
        std::atomic<int> state{0};
        std::uint64_t got = 0;
        std::thread w([&] {
            waiter::state st;
            p.ev.begin(st);
            bool first = true;
            std::uint64_t v = 0;
            while (!p.q.pop(v)) {
                if (first) {
                    first = false;
                    state.store(1);  // announced, looked, found nothing
                    while (state.load() != 2) std::this_thread::yield();
                }
                p.ev.step(st);
            }
            p.ev.end(st);
            got = v + 1;
            state.store(3);
        });
        while (state.load() != 1) std::this_thread::yield();
        p.submit(round);
        state.store(2);
        await([&] { return state.load() == 3; }, "a publish while the waiter was between its look and its sleep");
        w.join();
        if (got != static_cast<std::uint64_t>(round) + 1) {
            std::printf("wrong element\n");
            return 1;
        }
    }
    std::printf("interleave ok\n");
    return 0;
}

int stress(std::uint64_t n) {
    for (wait_policy pol : {wait_policy::spin, wait_policy::park, wait_policy::spin_then_park}) {
        for (int workers : {1, 4}) {
            for (int producers : {1, 3}) {
                pool p(pol, 1000);
                std::vector<std::atomic<std::uint8_t>> seen(n * producers);
                std::atomic<std::uint64_t> taken{0};
                std::vector<std::thread> ws;
                for (int w = 0; w < workers; ++w)
                    ws.emplace_back([&] {
                        std::uint64_t v;
                        while (p.take(v)) {
                            seen[v].fetch_add(1);
                            taken.fetch_add(1);
                        }
                    });
                std::vector<std::thread> ps;
                for (int k = 0; k < producers; ++k)
                    ps.emplace_back([&, k] {
                        std::mt19937 rng(k + 1);
                        for (std::uint64_t i = 0; i < n; ++i) {
                            if (rng() % 64 == 0) std::this_thread::sleep_for(std::chrono::microseconds(rng() % 50));
                            p.submit(k * n + i);
                        }
                    });
                for (auto &t : ps) t.join();
                await([&] { return taken.load() == n * producers; },
                      std::string("tasks queued and no waiter awake (") + name(pol) + ")");
                p.stop();
                for (auto &t : ws) t.join();
                for (std::uint64_t i = 0; i < n * producers; ++i)
                    if (seen[i].load() != 1) {
                        std::printf("task %llu taken %d times (%s)\n", static_cast<unsigned long long>(i),
                                    seen[i].load(), name(pol));
                        return 1;
                    }
            }
        }
    }
    std::printf("stress ok\n");
    return 0;
}

// Every waiter asleep (parks counted), then the stop flag and notify_all:
// every waiter must return, and none with a task.
int stop() {
    for (wait_policy pol : {wait_policy::spin, wait_policy::park, wait_policy::spin_then_park}) {
        pool p(pol, 1000);
        std::atomic<int> left{8};
        std::vector<std::thread> ws;
        for (int w = 0; w < 8; ++w)
            ws.emplace_back([&] {
                std::uint64_t v;
                if (p.take(v)) std::printf("a task from an empty pool\n"), std::_Exit(1);
                left.fetch_sub(1);
            });
        if (pol != wait_policy::spin)
            await([&] { return p.ev.waiting() == 8 && p.ev.parks() >= 8; }, "waiters never slept");
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        p.stop();
        await([&] { return left.load() == 0; }, std::string("the stop flag (") + name(pol) + ")");
        for (auto &t : ws) t.join();
        if (p.ev.waiting() != 0) {
            std::printf("waiters still counted after they returned\n");
            return 1;
        }
    }
    std::printf("stop ok\n");
    return 0;
}

// A waiter whose predicate stays false: with a yield budget it must park
// once the budget is spent (an idle pool sleeps), and be woken by the stop.
int idle() {
    pool p(wait_policy::spin_then_park, 4000, 64000);
    pool *pp = &p;
    std::thread w([pp] {
        std::uint64_t v;
        if (pp->take(v)) std::printf("a task from an empty pool\n"), std::_Exit(1);
    });
    await([&] { return p.ev.parks() >= 1; }, "a yielding waiter never parked");
    p.stop();
    w.join();
    std::printf("idle ok\n");
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "interleave") return interleave();
    if (mode == "stress") return stress(argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 10000);
    if (mode == "stop") return stop();
    if (mode == "idle") return idle();
    std::fprintf(stderr, "usage: waiter_loop_test interleave | stress [N] | stop | idle\n");
    return 2;
}
