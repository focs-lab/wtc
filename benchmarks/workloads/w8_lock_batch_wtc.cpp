// W8: what holding a mutex across a loop's critical sections is worth, and
// what it costs.
//
// T threads each run a loop that takes one mutex, updates a shared record,
// lets the mutex go and does W rounds of arithmetic of its own. The mutex
// is a global used directly, so the compiler can see the loop, and both
// builds come from this one source through tools/wtc-cc:
//
//   flavour   how it is built
//   plain     -DW8_PLAIN    the loop as written
//   batched   (nothing)     marked WTC_BATCH: the compiler holds the mutex
//                           across WTC_LOCK_BATCH_K sections at a time
//
// Holding the mutex longer makes the other threads wait longer for it, so
// beside the rate the result reports how unevenly the threads finish: the
// time from the first thread's end to the last's, as a share of the run.
// The record's sum must come out the same in every build.
//
//   w8_lock_batch_wtc [--n N] [--threads T] [--work W] [--cpus a,b,...]
//                     [--json F] [--flavour NAME] [--rep R]

#include "mutex.hpp"
#include "progress.hpp"
#include "thread.hpp"

#include "harness.hpp"

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

using namespace wtc_bench;

#ifdef W8_PLAIN
#define W8_LOOP
#else
#define W8_LOOP WTC_BATCH
#endif

namespace {

struct record {
    std::uint64_t sum;
    std::uint64_t count;
};

wtc::mutex m;
record shared{0, 0};
std::uint64_t g_n = 10'000'000;
std::uint64_t g_work = 0;
std::atomic<int> g_go{0};

// The loop the compiler may batch: nothing in it can wait for another
// thread, so it is a candidate; the arithmetic is the thread's own work
// between sections.
W8_LOOP std::uint64_t run(std::uint64_t seed) {
    std::uint64_t x = seed;
    for (std::uint64_t i = 0; i < g_n; ++i) {
        m.lock();
        shared.sum += i;
        ++shared.count;
        m.unlock();
        for (std::uint64_t w = 0; w < g_work; ++w) x = x * 6364136223846793005ull + 1442695040888963407ull;
    }
    return x;
}

struct slot {
    std::uint64_t end_ns;
    std::uint64_t out;
    char pad[48];
};
std::vector<slot> g_slots;

void worker(void* arg) {
    const std::size_t me = reinterpret_cast<std::size_t>(arg);
    while (!g_go.load(std::memory_order_acquire)) cpu_relax();
    g_slots[me].out = run(me + 1);
    g_slots[me].end_ns = now_ns();
}

} // namespace

int main(int argc, char** argv) {
    args a(argc, argv);
    g_n = a.getu("n", 10'000'000);
    g_work = a.getu("work", 0);
    const int threads = static_cast<int>(a.getu("threads", 2));
    const int rep = static_cast<int>(a.getu("rep", 0));
    const std::string json = a.get("json", "");
    const std::string flavour = a.get("flavour", "unknown");
    std::vector<int> cpus = choose_cpus(a, threads);
    g_slots.assign(threads, slot{});

    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) {
        ts.push_back(wtc::spawn(worker, reinterpret_cast<void*>(static_cast<std::size_t>(t))));
        pin_thread(ts.back(), cpus[t]);
    }
    const std::uint64_t start = now_ns();
    g_go.store(1, std::memory_order_release);
    for (auto& t : ts) t.join();

    std::uint64_t first = ~0ull, last = 0, mix = 0;
    for (const slot& s : g_slots) {
        first = std::min(first, s.end_ns);
        last = std::max(last, s.end_ns);
        mix ^= s.out;
    }
    const std::uint64_t want_count = g_n * static_cast<std::uint64_t>(threads);
    const std::uint64_t want_sum = static_cast<std::uint64_t>(threads) * (g_n * (g_n - 1) / 2);
    if (shared.count != want_count || shared.sum != want_sum) {
        std::fprintf(stderr, "lost updates: count %llu of %llu\n",
                     static_cast<unsigned long long>(shared.count), static_cast<unsigned long long>(want_count));
        return 1;
    }
    const double secs = static_cast<double>(last - start) / 1e9;
    const double msec = static_cast<double>(want_count) / secs / 1e6;
    const double spread = static_cast<double>(last - first) / static_cast<double>(last - start);

    result_line r;
    r.set("workload", std::string("W8"));
    r.set("variant", flavour);
    r.set("threads", threads);
    r.set("work", g_work);
    r.set("rep", rep);
    r.set("n_ops", want_count);
    r.set("mops", msec);
    r.set("finish_spread", spread);
    r.set("checksum", shared.sum);
    r.set("mix", mix);   // keeps each thread's own work from being removed
    r.write(json);
    std::printf("checksum %llu\n", static_cast<unsigned long long>(shared.sum));
    std::fprintf(stderr, "W8 %-8s T%d W%llu %8.2f M sections/s  finish spread %5.1f%%\n", flavour.c_str(),
                 threads, static_cast<unsigned long long>(g_work), msec, 100 * spread);
    return 0;
}
