// SPDX-License-Identifier: MIT
// The BS::thread_pool leg's program: tasks through one pool, each returning
// its result through the future that submit_task hands back.
//
//   bs-thread-pool-bench --workers W --rounds R [--seconds 0.4] [--inflight 4096]
//                        [--submitters S] [--detach 10000] [--tail 10000]
//                        [--cpus a,b,...] [--json F] [--rep R] [--variant NAME]
//                        [--work NS]
//   bs-thread-pool-bench --calibrate    rounds of work per ns, here and now
//
// One file, two programs. Built as it is, the main thread is the one
// thread that submits: the program whose queue has one producer. Built with
// -DTP_SUBMITTERS, --submitters threads (two by default), started in a loop,
// submit instead: the program whose queue has many.
//
// A task is the library's own: submit_task(f) takes a lambda holding the
// id, which does R rounds of dependent arithmetic (calibrated by the
// caller; --work is only recorded), adds the id to its thread's tally and
// returns a hash of the id. Each submitter keeps its last --inflight futures
// in a ring and, before reusing a slot, takes the future in it and checks
// its value, so at most that many of its tasks are in flight and every
// future is taken once, in order. A run is a warm-up of a tenth of
// --seconds, drained, then --seconds of submitting, drained; the rate is
// the measured tasks over the time from the start of the measured phase
// (every submitter past its warm-up) to the last future taken.
//
// Checks. After the measured phase the main thread detaches --detach more
// tasks and calls wait(); the tallies must then already count every task
// submitted, and the pool must count no task unfinished, queued or
// running, which is what wait() promises. Then it detaches --tail more and
// destroys the pool, whose destructor must wait for them. After the pool
// is destroyed and its workers joined: every future gave its id's hash,
// and the tallies of the pool's threads add up to the count, the sum of
// the ids, and the sum and the xor of the hashes of every id submitted, so
// every task ran exactly once. A failed run says why and exits 1. The
// pool's threads are pinned, by tasks run before the warm-up, to the first
// W processors of --cpus (or of W + S distinct cores), and the submitters
// to the next S.

#include "BS_thread_pool.hpp"

#include "harness.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <future>
#include <thread>
#include <vector>

using namespace wtc_bench;

namespace {

constexpr unsigned kMaxThreads = 64;
// The ids of the tasks detached before wait(), apart from the submitters'.
constexpr std::uint64_t kDetached = std::uint64_t{1} << 48;
// The ids of the tasks left queued for the destructor.
constexpr std::uint64_t kTail = std::uint64_t{1} << 49;

// What a pool thread's tasks add up to, each on its own lines.
struct alignas(128) tally {
    std::uint64_t count = 0, ids = 0, hash_sum = 0, hash_xor = 0, sink = 0;
};

tally g_tally[kMaxThreads];
std::atomic<unsigned> g_tally_next{0};
thread_local tally* t_tally = nullptr;
std::uint64_t g_rounds = 0;

inline std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

std::uint64_t task(std::uint64_t id) {
    tally* t = t_tally;
    if (!t) {
        const unsigned k = g_tally_next.fetch_add(1, std::memory_order_relaxed);
        if (k >= kMaxThreads) { std::fprintf(stderr, "more than %u pool threads ran tasks\n", kMaxThreads); std::abort(); }
        t = t_tally = &g_tally[k];
    }
    t->sink += work_rounds(id, g_rounds);
    const std::uint64_t h = mix(id);
    ++t->count; t->ids += id; t->hash_sum += h; t->hash_xor ^= h;
    return h;
}

// Pins the pool's threads, one to each of the first W processors: W tasks
// that each pin the thread running it and wait until all W have started,
// so that no thread runs two of them.
void pin_pool(BS::thread_pool<>& pool, std::size_t workers, const std::vector<int>& cpus) {
    std::atomic<std::size_t> started{0};
    std::vector<std::future<int>> f;
    for (std::size_t w = 0; w < workers; ++w) {
        f.push_back(pool.submit_task([&started, &cpus, workers] {
            pin_self(cpus[started.fetch_add(1)]);
            while (started.load() < workers) cpu_relax();
            return 0;
        }));
    }
    for (auto& x : f) x.get();
}

// One thread's submitting: ids first, first + stride, ... in order. On
// lines of its own: the submitters write their counts on every task.
struct alignas(128) submitter {
    BS::thread_pool<>* pool;
    std::uint64_t first, stride;
    std::uint64_t next = 0, bad = 0, measured = 0, t_done = 0;
    std::vector<std::future<std::uint64_t>> fut;
    std::vector<std::uint64_t> id;

    submitter(BS::thread_pool<>* p, std::uint64_t first, std::uint64_t stride, std::size_t inflight)
        : pool(p), first(first), stride(stride), fut(inflight), id(inflight) {}

    void take(std::size_t k) {
        if (fut[k].get() != mix(id[k])) ++bad;
    }

    // Submits until the deadline, then takes every future still out;
    // returns the tasks submitted.
    std::uint64_t phase(std::uint64_t deadline) {
        const std::size_t b = fut.size();
        std::uint64_t i = 0;
        for (;; ++i) {
            if ((i & 255) == 0 && now_ns() >= deadline) break;
            const std::size_t k = i % b;
            if (i >= b) take(k);
            id[k] = first + stride * next++;
            fut[k] = pool->submit_task([i = id[k]] { return task(i); });
        }
        for (std::uint64_t r = i > b ? i - b : 0; r < i; ++r) take(r % b);
        return i;
    }
};

struct start_line {
    std::atomic<unsigned> arrived{0};
    std::atomic<std::uint64_t> t_start{0};
};

// Warm-up, then wait for every submitter, then the measured phase.
void run(submitter& s, unsigned submitters, std::uint64_t warm_end, std::uint64_t run_ns, start_line& line) {
    s.phase(warm_end);
    if (line.arrived.fetch_add(1) + 1 == submitters) line.t_start.store(now_ns());
    std::uint64_t t0;
    while ((t0 = line.t_start.load()) == 0) cpu_relax();
    s.measured = s.phase(t0 + run_ns);
    s.t_done = now_ns();
}

}  // namespace

int main(int argc, char** argv) {
    args a(argc, argv);
    if (a.has("calibrate")) {
        std::vector<double> r;
        for (int i = 0; i < 5; ++i) r.push_back(work_rounds_per_ns());
        std::sort(r.begin(), r.end());
        std::printf("%.4f\n", r[2]);
        return 0;
    }
    const std::size_t workers = a.getu("workers", 1);
#ifdef TP_SUBMITTERS
    const unsigned submitters = static_cast<unsigned>(a.getu("submitters", 2));
#else
    const unsigned submitters = 1;
#endif
    g_rounds = a.getu("rounds", 0);
    const std::uint64_t work_ns = a.getu("work", 0);
    const double seconds = std::stod(a.get("seconds", "0.4"));
    const std::size_t inflight = a.getu("inflight", 4096);
    const std::string json = a.get("json", "");
    const std::string variant = a.get("variant", "bs-thread-pool");
    const std::uint64_t detach = a.getu("detach", 10000);
    const std::uint64_t tail = a.getu("tail", 10000);
    const int rep = static_cast<int>(a.getu("rep", 0));
    const std::vector<int> cpus = choose_cpus(a, static_cast<int>(workers + submitters));
    pin_self(cpus[workers]);

    const std::uint64_t run_ns = static_cast<std::uint64_t>(seconds * 1e9);
    std::vector<submitter> subs;
    start_line line;
    std::uint64_t t_start = 0, t_end = 0;
    {
        BS::thread_pool<> pool(workers);
        pin_pool(pool, workers, cpus);
        for (unsigned s = 0; s < submitters; ++s) subs.emplace_back(&pool, s, submitters, inflight);
        const std::uint64_t warm_end = now_ns() + std::max<std::uint64_t>(run_ns / 10, 20'000'000);
#ifdef TP_SUBMITTERS
        std::vector<std::thread> threads;
        for (unsigned s = 0; s < submitters; ++s) {
            threads.emplace_back([&, s] {
                pin_self(cpus[workers + s]);
                run(subs[s], submitters, warm_end, run_ns, line);
            });
        }
        for (auto& t : threads) t.join();
#else
        run(subs[0], submitters, warm_end, run_ns, line);
#endif
        t_start = line.t_start.load();
        for (auto& s : subs) t_end = std::max(t_end, s.t_done);

        // wait(): once it returns, every task has run.
        for (std::uint64_t j = 0; j < detach; ++j) pool.detach_task([i = kDetached + j] { task(i); });
        pool.wait();
        tally now;
        const unsigned threads_now = g_tally_next.load();
        for (unsigned k = 0; k < threads_now; ++k) now.count += g_tally[k].count;
        std::uint64_t all = detach;
        for (auto& s : subs) all += s.next;
        if (now.count != all) {
            std::fprintf(stderr, "bs-thread-pool %s: wait() returned with %llu of %llu tasks run\n", variant.c_str(),
                         (unsigned long long)now.count, (unsigned long long)all);
            return 1;
        }
        if (pool.get_tasks_total() || pool.get_tasks_queued() || pool.get_tasks_running()) {
            std::fprintf(stderr, "bs-thread-pool %s: after wait(), %zu tasks unfinished, %zu queued, %zu running\n",
                         variant.c_str(), pool.get_tasks_total(), pool.get_tasks_queued(), pool.get_tasks_running());
            return 1;
        }

        // Shutdown: the destructor waits for the tasks still queued.
        for (std::uint64_t j = 0; j < tail; ++j) pool.detach_task([i = kTail + j] { task(i); });
    }

    // The checks.
    std::uint64_t measured = 0, submitted = 0, bad = 0;
    tally want, got;
    for (auto& s : subs) {
        measured += s.measured;
        submitted += s.next;
        bad += s.bad;
        for (std::uint64_t j = 0; j < s.next; ++j) {
            const std::uint64_t i = s.first + s.stride * j, h = mix(i);
            ++want.count; want.ids += i; want.hash_sum += h; want.hash_xor ^= h;
        }
    }
    for (std::uint64_t j = 0; j < detach; ++j) {
        const std::uint64_t i = kDetached + j, h = mix(i);
        ++want.count; want.ids += i; want.hash_sum += h; want.hash_xor ^= h;
    }
    for (std::uint64_t j = 0; j < tail; ++j) {
        const std::uint64_t i = kTail + j, h = mix(i);
        ++want.count; want.ids += i; want.hash_sum += h; want.hash_xor ^= h;
    }
    const unsigned ran = g_tally_next.load();
    for (unsigned k = 0; k < ran; ++k) {
        const tally& t = g_tally[k];
        got.count += t.count; got.ids += t.ids; got.hash_sum += t.hash_sum; got.hash_xor ^= t.hash_xor;
    }
    const char* why = nullptr;
    if (ran > workers) why = "tasks ran on more threads than the pool has";
    else if (bad) why = "a future gave a wrong value";
    else if (got.count != want.count) why = "the pool ran another number of tasks than were submitted";
    else if (got.ids != want.ids || got.hash_sum != want.hash_sum || got.hash_xor != want.hash_xor)
        why = "the pool ran other tasks than were submitted";
    if (why) {
        std::fprintf(stderr, "bs-thread-pool %s: %s (%llu submitted, %llu ran, %llu bad futures, %u threads)\n",
                     variant.c_str(), why, (unsigned long long)want.count, (unsigned long long)got.count,
                     (unsigned long long)bad, ran);
        return 1;
    }

    const double secs = static_cast<double>(t_end - t_start) / 1e9;
    result_line r;
    r.set("workload", std::string("bs-thread-pool"));
    r.set("detached", detach);
    r.set("tail", tail);
    r.set("variant", variant);
    r.set("workers", static_cast<std::uint64_t>(workers));
    r.set("submitters", static_cast<std::uint64_t>(submitters));
    r.set("work_ns", work_ns);
    r.set("rounds", g_rounds);
    r.set("inflight", static_cast<std::uint64_t>(inflight));
    r.set("rep", rep);
    r.set("tasks", measured);
    r.set("submitted", submitted);
    r.set("seconds", secs);
    r.set("mops", static_cast<double>(measured) / secs / 1e6);
    r.set("check", std::string("ok"));
    r.write(json);
    std::fprintf(stderr, "bs-thread-pool %-12s %u submitters %2zu workers work %5llu ns  %8.3f M tasks/s  (%llu tasks, checked)\n",
                 variant.c_str(), submitters, workers, (unsigned long long)work_ns,
                 static_cast<double>(measured) / secs / 1e6, (unsigned long long)measured);
    return 0;
}
