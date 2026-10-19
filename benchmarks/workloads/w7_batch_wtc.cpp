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
// that the compiler batches its loop. -DW7_ELEM=E8 or E64 picks the
// element. The checksum must agree across all builds of one element type.
//
// Batching delays an element by up to WTC_BATCH_K pushes, so the latency
// it reports is part of the result, not a side note.
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

std::uint64_t g_n = 10'000'000;
std::uint64_t g_t_start = 0, g_t_end = 0, g_checksum = 0;
bool g_order_ok = true;
std::vector<std::uint64_t> g_ticks;

W7_PRODUCER void produce() {
    elem e{};
    for (std::uint64_t i = 0; i < g_n; ++i) {
        e.seq = i;
        e.set_tsc(rdtsc());
        q.push(e);
    }
}

void consume() {
    std::uint64_t sum = 0;
    for (std::uint64_t got = 0; got < g_n;) {
        auto p = q.try_pop();
        if (!p) { cpu_relax(); continue; }
        const std::uint64_t t = rdtsc();
        if (p->seq != got) { g_order_ok = false; break; }
        sum += p->seq * 0x9E3779B97F4A7C15ull;
        if (p->tsc() && (got & 63) == 0) g_ticks.push_back(t - p->tsc());
        ++got;
    }
    g_t_end = now_ns();
    g_checksum = sum;
}

} // namespace

int main(int argc, char** argv) {
    args a(argc, argv);
    g_n = a.getu("n", 10'000'000);
    const int rep = static_cast<int>(a.getu("rep", 0));
    const std::string json = a.get("json", "");
    const std::string flavour = a.get("flavour", "unknown");
    std::vector<int> cpus = choose_cpus(a, 2);
    const double tsc_hz = calibrate_tsc_hz();
    g_ticks.reserve(g_n / 64 + 1);

    g_t_start = now_ns();
    std::thread c = wtc::spawn(consume);
    std::thread p = wtc::spawn(produce);
    pin_thread(c, cpus[1]);
    pin_thread(p, cpus[0]);
    p.join();
    c.join();

    if (!g_order_ok) { std::fprintf(stderr, "FIFO broken\n"); return 1; }
    const double secs = static_cast<double>(g_t_end - g_t_start) / 1e9;
    const double mops = static_cast<double>(g_n) / secs / 1e6;
    percentiles pc = compute_percentiles(g_ticks, 1e9 / tsc_hz);
    const char* tag = q.tag() == wtc::topology::spsc_inline ? "spsc_inline"
                      : q.tag() == wtc::topology::spsc    ? "spsc"
                                                          : "generic";

    result_line r;
    r.set("workload", std::string("W7"));
    r.set("variant", flavour);
    r.set("elem", std::string(sizeof(elem) == 8 ? "8" : "64"));
    r.set("rep", rep);
    r.set("n_ops", g_n);
    r.set("mops", mops);
    r.set("p50", pc.p50); r.set("p99", pc.p99); r.set("p999", pc.p999); r.set("max_ns", pc.max);
    r.set("checksum", g_checksum);
    r.set("tags", std::string(tag));
    r.write(json);
    std::printf("checksum %llu tag %s\n", static_cast<unsigned long long>(g_checksum), tag);
    std::fprintf(stderr, "W7 %-14s E%zu %8.2f Mops/s  p50 %7.0f  p99 %8.0f ns\n", flavour.c_str(),
                 sizeof(elem), mops, pc.p50, pc.p99);
    return 0;
}
