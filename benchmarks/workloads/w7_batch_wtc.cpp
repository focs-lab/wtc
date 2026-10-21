// W7: what batching a producer's pushes is worth, and what it costs.
//
// A producer pushes n elements in a plain loop, as fast as it can; one
// consumer pops them, checks their order and, for 64-byte elements, how
// long each waited between its push and its pop. Like W3wtc the queue is a
// global used directly, so the compiler can reason about it, and every
// build comes from this one source through tools/wtc-cc:
//
//   implementation        how it is built
//   generic               --no-specialize
//   spsc                  --no-specialize -DW7_TAG=1    the source declares it
//   inline                (nothing)                     the compiler chooses it
//
// each with and without -DW7_BATCH, which marks the producer WTC_BATCH so
// that the compiler batches its loop, and with -DW7_HAND_BATCH, the same
// batch written out by hand through the shim's batch calls: what a
// programmer who knew the trick would write, and so the bound for what the
// compiler's batching can be expected to reach. -DW7_ELEM=E8 or E64 picks
// the element; -DWTC_BATCH_K sets the batch size (64 by default), which
// every results line records. The checksum must agree across all builds of
// one element type.
//
// Batching delays an element by up to WTC_BATCH_K pushes, so the latency
// it reports is part of the result, not a side note. As fast as it can (the
// default) the producer runs ahead of the consumer and latency measures the
// backlog; --rate R paces it, one element every 1/R seconds by
// busy-waiting on the timestamp counter, and latency then runs from each
// element's due time, which is where batching's price shows: an element
// waits for the rest of its batch to be pushed. The consumer times the
// run from the first element after warm-up to the last: the producer's loop
// is what the compiler batches, and a call into the clock there would be a
// call it cannot see, which its rules refuse to batch.
//
//   w7_batch_wtc [--n N] [--cpus a,b] [--json F] [--flavour NAME] [--rep R]

#include "progress.hpp"
#include "queue.hpp"
#include "thread.hpp"

#include "elements.hpp"
#include "harness.hpp"

#include <thread>
#include <vector>

using namespace wtc_bench;

#ifndef W7_ELEM
#define W7_ELEM E64
#endif
#ifndef W7_TAG
#define W7_TAG 0
#endif
#ifdef W7_BATCH
#define W7_PRODUCER WTC_BATCH
#else
#define W7_PRODUCER
#endif

namespace {

using elem = W7_ELEM;
wtc::queue<elem> q(static_cast<wtc::topology>(W7_TAG));

std::uint64_t g_n = 10'000'000, g_warm = 1'000'000, g_interval = 0;
std::uint64_t g_t_start = 0, g_t_end = 0, g_checksum = 0;
bool g_order_ok = true;
std::vector<std::uint64_t> g_ticks;

#ifdef W7_HAND_BATCH
void produce() {
    elem e{};
    wtc::queue<elem>::batch b;
    std::uint64_t next = rdtsc();
    q.batch_begin(b);
    for (std::uint64_t i = 0; i < g_n; ++i) {
        e.seq = i;
        if (g_interval) {
            while (rdtsc() < next) cpu_relax();
            e.set_tsc(next);
            next += g_interval;
        } else {
            e.set_tsc(rdtsc());
        }
        q.push_deferred(b, e);
    }
    q.batch_end(b);
}
#else
W7_PRODUCER void produce() {
    elem e{};
    std::uint64_t next = rdtsc();
    for (std::uint64_t i = 0; i < g_n; ++i) {
        e.seq = i;
        if (g_interval) {
            while (rdtsc() < next) cpu_relax();
            e.set_tsc(next);
            next += g_interval;
        } else {
            e.set_tsc(rdtsc());
        }
        q.push(e);
    }
}
#endif

void consume() {
    // Samples go to a vector of the consumer's own, handed over at the end:
    // g_ticks shares a cache line with g_interval, which the producer reads
    // on every element.
    std::vector<std::uint64_t> ticks = std::move(g_ticks);
    std::uint64_t sum = 0;
    for (std::uint64_t got = 0; got < g_n;) {
        auto p = q.try_pop();
        if (!p) { cpu_relax(); continue; }
        const std::uint64_t t = rdtsc_fenced();
        if (p->seq != got) { g_order_ok = false; break; }
        if (got == g_warm) g_t_start = now_ns();
        sum += p->seq * 0x9E3779B97F4A7C15ull;
        // Every 61st element: a prime, so the sample never lines up with a
        // batch boundary. Every 64th, as this once was, sampled only the
        // first element of each batch of 64, the one that waits longest.
        if (got >= g_warm && p->tsc() && got % 61 == 0) ticks.push_back(t - p->tsc());
        ++got;
    }
    g_t_end = now_ns();
    g_checksum = sum;
    g_ticks = std::move(ticks);
}

} // namespace

int main(int argc, char** argv) {
    args a(argc, argv);
    g_n = a.getu("n", 10'000'000);
    g_warm = a.getu("warm", g_n / 10);
    const int rep = static_cast<int>(a.getu("rep", 0));
    const std::string json = a.get("json", "");
    const std::string flavour = a.get("flavour", "unknown");
    std::vector<int> cpus = choose_cpus(a, 2);
    const double tsc_hz = calibrate_tsc_hz();
    const std::uint64_t rate = a.getu("rate", 0);
    if (rate) g_interval = static_cast<std::uint64_t>(tsc_hz / static_cast<double>(rate));
    g_ticks.reserve(g_n / 61 + 1);

    std::thread c = wtc::spawn(consume);
    std::thread p = wtc::spawn(produce);
    pin_thread(c, cpus[1]);
    pin_thread(p, cpus[0]);
    const long f0 = cur_freq_khz(cpus[0]);
    p.join();
    c.join();
    const long f1 = cur_freq_khz(cpus[0]);

    if (!g_order_ok) { std::fprintf(stderr, "FIFO broken\n"); return 1; }
    const double secs = static_cast<double>(g_t_end - g_t_start) / 1e9;
    const double mops = static_cast<double>(g_n - g_warm) / secs / 1e6;
    percentiles pc = compute_percentiles(g_ticks, 1e9 / tsc_hz);
    const char* tag = q.tag() == wtc::topology::spsc_inline ? "spsc_inline"
                      : q.tag() == wtc::topology::spsc    ? "spsc"
                                                          : "generic";

    result_line r;
    r.set("workload", std::string("W7"));
    r.set("mode", rate ? std::string("open") : std::string("closed"));
    r.set("rate", rate);
    if (rate) r.set("saturated", mops * 1e6 < 0.95 * static_cast<double>(rate) ? 1 : 0);
    r.set("variant", flavour);
    r.set("elem", std::string(sizeof(elem) == 8 ? "8" : "64"));
    r.set("rep", rep);
    r.set("n_ops", g_n - g_warm);
    r.set("n_total", g_n);
    r.set("mops", mops);
    r.set("p50", pc.p50); r.set("p90", pc.p90); r.set("p99", pc.p99);
    r.set("p999", pc.p999); r.set("max_ns", pc.max);
    if (!g_ticks.empty()) r.set("hist_ns", log_histogram(g_ticks, 1e9 / tsc_hz));
    r.set("checksum", g_checksum);
    r.set("tags", std::string(tag));
#if defined(W7_BATCH) || defined(W7_HAND_BATCH)
    r.set("k", static_cast<std::uint64_t>(WTC_BATCH_K));
#else
    r.set("k", static_cast<std::uint64_t>(1));
#endif
    r.set("cpus", std::to_string(cpus[0]) + "," + std::to_string(cpus[1]));
    r.set("freq_before_khz", static_cast<std::uint64_t>(f0));
    r.set("freq_after_khz", static_cast<std::uint64_t>(f1));
    r.set("tsc_hz", tsc_hz);
    r.write(json);
    std::printf("checksum %llu tag %s\n", static_cast<unsigned long long>(g_checksum), tag);
    std::fprintf(stderr, "W7 %-14s E%zu %8.2f Mops/s  p50 %7.0f  p99 %8.0f ns\n", flavour.c_str(),
                 sizeof(elem), mops, pc.p50, pc.p99);
    return 0;
}
