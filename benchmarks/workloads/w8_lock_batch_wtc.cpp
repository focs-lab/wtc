// W8: what holding a mutex across a loop's critical sections is worth, and
// what it costs.
//
// T threads each run a loop that takes one mutex, updates a shared record,
// lets the mutex go and does some work of its own: rounds of a xorshift-
// multiply step, which the compiler cannot fold the way it folded the
// linear step this workload used before (eight rounds into one multiply).
// --work-ns asks for a duration instead of a round count; the rounds are
// calibrated against the clock first, and both are recorded. The mutex is
// a global used directly, so the compiler can see the loop, and every build
// comes from this one source through tools/wtc-cc:
//
//   flavour   how it is built
//   plain     -DW8_PLAIN        the loop as written
//   batched   (nothing)         marked WTC_BATCH: the compiler holds the
//                               mutex across WTC_LOCK_BATCH_K sections
//   hand      -DW8_HAND_BATCH   the same batch written out through the
//                               shim's batch calls: the bound for the
//                               compiler's
//
// -DWTC_LOCK_BATCH_K sets the batch size (64 by default); every line
// records it. Holding the mutex longer makes the other threads wait longer
// for it, so beside the rate the result reports how evenly the threads
// progress -- Jain's fairness index over each thread's rate (every thread
// does the same number of sections, so its rate is that over its finish
// time), and the time from the first thread's end to the last's -- and the
// distribution of one section's time, wait included, sampled every 61st
// section -- a prime, so the sample does not line up with the batch
// boundaries, where the waits are. The record's sum must come out the same
// in every build.
//
//   w8_lock_batch_wtc [--n N] [--threads T] [--work R | --work-ns NS]
//                     [--cpus a,b,...] [--json F] [--flavour NAME] [--rep R]

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

inline std::uint64_t step(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 29;
    return x;
}

// The loop the compiler may batch: nothing in it can wait for another
// thread, so it is a candidate; the arithmetic is the thread's own work
// between sections, and every 61st section is timed into the caller's
// array with the timestamp counter, which is an instruction, not a call.
#ifdef W8_HAND_BATCH
std::uint64_t run(std::uint64_t seed, std::uint64_t* samples) {
    std::uint64_t x = seed, t0 = 0;
    wtc::mutex::batch b;
    m.batch_begin(b);
    for (std::uint64_t i = 0; i < g_n; ++i) {
        if (i % 61 == 0) t0 = rdtsc();
        shared.sum += i;
        ++shared.count;
        m.batch_step(b);
        if (i % 61 == 0) samples[i / 61] = rdtsc() - t0;
        for (std::uint64_t w = 0; w < g_work; ++w) x = step(x);
    }
    m.batch_end(b);
    return x;
}
#else
W8_LOOP std::uint64_t run(std::uint64_t seed, std::uint64_t* samples) {
    std::uint64_t x = seed, t0 = 0;
    for (std::uint64_t i = 0; i < g_n; ++i) {
        if (i % 61 == 0) t0 = rdtsc();
        m.lock();
        shared.sum += i;
        ++shared.count;
        m.unlock();
        if (i % 61 == 0) samples[i / 61] = rdtsc() - t0;
        for (std::uint64_t w = 0; w < g_work; ++w) x = step(x);
    }
    return x;
}
#endif

struct slot {
    std::uint64_t end_ns;
    std::uint64_t out;
    std::vector<std::uint64_t> samples;
};
std::vector<slot> g_slots;

void worker(void* arg) {
    const std::size_t me = reinterpret_cast<std::size_t>(arg);
    while (!g_go.load(std::memory_order_acquire)) cpu_relax();
    g_slots[me].out = run(me + 1, g_slots[me].samples.data());
    g_slots[me].end_ns = now_ns();
}

// Rounds of the work step per nanosecond on this machine, measured on the
// main thread before any worker starts.
double rounds_per_ns() {
    const std::uint64_t rounds = 20'000'000;
    std::uint64_t x = 1;
    const std::uint64_t t0 = now_ns();
    for (std::uint64_t i = 0; i < rounds; ++i) x = step(x);
    const std::uint64_t t1 = now_ns();
    if (x == 0) std::fputc(' ', stderr);   // keeps the loop
    return static_cast<double>(rounds) / static_cast<double>(t1 - t0);
}

} // namespace

int main(int argc, char** argv) {
    args a(argc, argv);
    g_n = a.getu("n", 10'000'000);
    const double per_ns = rounds_per_ns();
    // What the leg asked for is the configuration, so it is what "work"
    // records; the rounds it came to on this run vary with the calibration.
    std::string work_asked;
    if (a.has("work-ns")) {
        g_work = static_cast<std::uint64_t>(static_cast<double>(a.getu("work-ns", 0)) * per_ns + 0.5);
        work_asked = std::to_string(a.getu("work-ns", 0)) + "ns";
    } else {
        g_work = a.getu("work", 0);
        work_asked = std::to_string(g_work);
    }
    const int threads = static_cast<int>(a.getu("threads", 2));
    const int rep = static_cast<int>(a.getu("rep", 0));
    const std::string json = a.get("json", "");
    const std::string flavour = a.get("flavour", "unknown");
    std::vector<int> cpus = choose_cpus(a, threads);
    const double tsc_hz = calibrate_tsc_hz();
    g_slots.assign(threads, slot{});
    for (auto& s : g_slots) s.samples.assign(g_n / 61 + 1, 0);

    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) {
        ts.push_back(wtc::spawn(worker, reinterpret_cast<void*>(static_cast<std::size_t>(t))));
        pin_thread(ts.back(), cpus[t]);
    }
    const long f0 = cur_freq_khz(cpus[0]);
    const std::uint64_t start = now_ns();
    g_go.store(1, std::memory_order_release);
    for (auto& t : ts) t.join();
    const long f1 = cur_freq_khz(cpus[0]);

    std::uint64_t first = ~0ull, last = 0, mix = 0;
    double rate_sum = 0, rate_sq = 0;
    std::vector<std::uint64_t> ticks;
    for (const slot& s : g_slots) {
        first = std::min(first, s.end_ns);
        last = std::max(last, s.end_ns);
        mix ^= s.out;
        const double rate = static_cast<double>(g_n) / static_cast<double>(s.end_ns - start);
        rate_sum += rate;
        rate_sq += rate * rate;
        ticks.insert(ticks.end(), s.samples.begin(), s.samples.end());
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
    const double jain = rate_sum * rate_sum / (static_cast<double>(threads) * rate_sq);
    percentiles pc = compute_percentiles(ticks, 1e9 / tsc_hz);

    result_line r;
    r.set("workload", std::string("W8"));
    r.set("variant", flavour);
    r.set("threads", threads);
    r.set("work", work_asked);
    r.set("work_rounds", g_work);
    r.set("work_ns", static_cast<double>(g_work) / per_ns);
    r.set("rep", rep);
    r.set("n_ops", want_count);
    r.set("n_total", want_count);
    r.set("mops", msec);
    r.set("finish_spread", spread);
    r.set("jain_index", jain);
    r.set("p50", pc.p50); r.set("p90", pc.p90); r.set("p99", pc.p99);
    r.set("p999", pc.p999); r.set("max_ns", pc.max);
    r.set("hist_ns", log_histogram(ticks, 1e9 / tsc_hz));
#ifdef W8_PLAIN
    r.set("k", static_cast<std::uint64_t>(1));
#else
    r.set("k", static_cast<std::uint64_t>(WTC_LOCK_BATCH_K));
#endif
    std::string cpu_list;
    for (int c : cpus) cpu_list += (cpu_list.empty() ? "" : ",") + std::to_string(c);
    r.set("cpus", cpu_list);
    r.set("freq_before_khz", static_cast<std::uint64_t>(f0));
    r.set("freq_after_khz", static_cast<std::uint64_t>(f1));
    r.set("tsc_hz", tsc_hz);
    r.set("checksum", shared.sum);
    r.set("mix", mix);   // keeps each thread's own work from being removed
    r.write(json);
    std::printf("checksum %llu\n", static_cast<unsigned long long>(shared.sum));
    std::fprintf(stderr, "W8 %-12s T%d work %.0f ns %8.2f M sections/s  Jain %.3f  spread %5.1f%%  p99 %6.0f ns\n",
                 flavour.c_str(), threads, static_cast<double>(g_work) / per_ns, msec, jain, 100 * spread, pc.p99);
    return 0;
}
