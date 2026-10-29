// A pool of workers that sleep while there is nothing to do: one thread
// submits tasks to a queue the workers take them from with pop_wait, and
// close() tells the workers, once the tasks run out, that no more will
// come. The queue is built with a wait policy (WAIT_POLICY: 0 spin, 1 park,
// 2 spin_then_park; park by default); the topology is left to the compiler,
// which proves one producer and many consumers and chooses spmc.
//
// Between rounds the submitter pauses long enough for every worker to fall
// asleep, so that each round's first tasks have to wake them. Every task
// must run exactly once, and the workers must all return once the queue is
// closed and drained.

#include "queue.hpp"
#include "thread.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#ifndef WAIT_POLICY
#define WAIT_POLICY 1
#endif

struct task {
    int id;
};

wtc::queue<task> tasks(wtc::topology::generic, static_cast<wtc::wait_policy>(WAIT_POLICY));

constexpr int kWorkers = 4;
constexpr int kRounds = 20;
constexpr int kPerRound = 500;
constexpr int kTotal = kRounds * kPerRound;

std::atomic<unsigned char> ran[kTotal];
std::atomic<int> done{0};
std::atomic<bool> ok{true};

void worker() {
    task t{};
    while (tasks.pop_wait(t)) {
        if (t.id < 0 || t.id >= kTotal || ran[t.id].exchange(1)) ok = false;
        done.fetch_add(1, std::memory_order_relaxed);
    }
}

int main() {
    std::vector<std::thread> ws;
    for (int w = 0; w < kWorkers; ++w) ws.push_back(wtc::spawn(worker));
    for (int r = 0; r < kRounds; ++r) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        for (int i = 0; i < kPerRound; ++i) tasks.push(task{r * kPerRound + i});
    }
    tasks.close();
    for (auto& w : ws) w.join();
    std::printf("tag %d policy %d %s %d\n", static_cast<int>(tasks.tag()),
                static_cast<int>(tasks.policy()), ok && done.load() == kTotal ? "ok" : "LOST OR TWICE",
                done.load());
}
