// W13: many producers and one consumer, with and without an order between
// the producers.
//
//   w13_mpsc_order --variant shim_laned_move --producers 8 [--elem 8|64|256]
//                  [--n N] [--cpus c,p0,p1,...] [--json runs.jsonl] [--rep r]
//   w13_mpsc_order ... --interval-ns 8000 [--burst 1] [--sleep]
//
// The question is how much a queue that keeps each producer's order and no
// order between producers (runtime/laned_mpsc.hpp) gains over the
// linearisable ones, and so whether relaxing the order where no thread can
// observe it is worth the compiler's while. Every variant holds the element
// in its cell and is popped by moving it out (adapters.hpp, W13), so what
// differs is the queue.
//
// stream (no --interval-ns): P producers push n elements each as fast as
// they can, with about 8192 at most in flight, as in W9, so that an
// unbounded queue measures the queue and not the memory it grows into.
// Throughput is elements delivered a second, from the first producer's
// n/10-th push to the last pop. One push in 61 and one poll in 61 are
// timed, each between two fenced reads of the TSC (the same overhead for
// every variant): a push's cost, a successful pop's and an empty poll's.
// The TSC is read nowhere else for 8-byte elements, which carry no stamp,
// so that their throughput is the queue's and not the clock's.
//
// paced (--interval-ns): each producer pushes a burst of --burst elements
// every interval, the producers' schedules staggered evenly over it, and
// waits for the next by spinning on the TSC, or with --sleep by sleeping
// until it. A sleeping producer needs no core of its own, so many of them
// can share a few cores: that is the sparse case, where a consumer that
// searches its producers one by one pays for every idle one.
//
// In both, each element's latency runs from its push to its pop (64- and
// 256-byte elements, which carry the stamp), every element carries its
// producer in its top byte, and the consumer checks that each producer's
// elements arrive exactly once and in order. The consumer runs on the first
// cpu given, producer p on cpu 1 + p mod (k - 1).

#include "adapters.hpp"
#include "elements.hpp"
#include "harness.hpp"

#include <thread>
#include <time.h>

using namespace wtc_bench;

namespace {

// The laned queue straight from the runtime, with a burst other than the
// shim's 64: how long the consumer stays on one lane.
template <class T, unsigned kBurst, bool kSlip = true> struct laned_burst {
    wtc::rt::laned_mpsc<T, kBurst, wtc::rt::lane_slots<T>(), 64, kSlip> q;
    void push(const T& v) { q.push(v); }
    bool try_pop(T& out) { return q.try_pop(out); }
};

template <class T, class F>
bool with_mpsc_variant(const std::string& name, F&& f) {
    if (name == "shim_laned_move")      { f.template run<shim_laned_move<T>>(); return true; }
    if (name == "shim_mpsc_move")       { f.template run<shim_mpsc_move<T>>(); return true; }
    if (name == "shim_generic_move")    { f.template run<shim_generic_move<T>>(); return true; }
    if (name == "moodycamel_cq_inline") { f.template run<moodycamel_cq_inline<T>>(); return true; }
    if (name == "std_mutex_inline")     { f.template run<std_mutex_inline<T>>(); return true; }
    if (name == "laned_k1")             { f.template run<laned_burst<T, 1>>(); return true; }
    if (name == "laned_k8")             { f.template run<laned_burst<T, 8>>(); return true; }
    if (name == "laned_k512")           { f.template run<laned_burst<T, 512>>(); return true; }
    if (name == "laned_noslip")         { f.template run<laned_burst<T, 64, false>>(); return true; }
    if (name == "laned_noslip_k512")    { f.template run<laned_burst<T, 512, false>>(); return true; }
    return false;
}

struct config {
    std::string variant, elem, json;
    std::uint64_t n, interval_ns, burst;
    bool sleep;
    int producers;
    std::vector<int> cpus;
    double tsc_hz;
    int rep;
};

void sleep_until_ns(std::uint64_t t) {
    timespec ts;
    ts.tv_sec = static_cast<time_t>(t / 1'000'000'000);
    ts.tv_nsec = static_cast<long>(t % 1'000'000'000);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) != 0) {}
}

std::uint64_t monotonic_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000 + static_cast<std::uint64_t>(ts.tv_nsec);
}

template <class E>
struct runner {
    const config& c;
    explicit runner(const config& cfg) : c(cfg) {}

    template <class Q> void run() {
        auto q = std::make_unique<Q>();
        const int P = c.producers;
        const bool paced = c.interval_ns > 0;
        const std::uint64_t n = c.n, warm = n / 10, total = n * static_cast<std::uint64_t>(P);
        const std::uint64_t burst = c.burst ? c.burst : 1;
        const double ns_per_tick = 1e9 / c.tsc_hz;
        // Each on a line of its own: the consumer writes popped, the
        // producers pushed, and neither should take the other's line.
        struct alignas(64) counter { std::atomic<std::uint64_t> v{0}; };
        counter popped_line, pushed_line;
        std::atomic<std::uint64_t>& popped = popped_line.v;
        std::atomic<std::uint64_t>& pushed = pushed_line.v;
        std::atomic<std::uint64_t> t_start{0};
        std::atomic<int> started{0};
        std::atomic<bool> go{false};
        std::uint64_t go_tsc = 0, go_ns = 0;
        std::vector<std::vector<std::uint64_t>> push_ticks(P);

        auto producer = [&](int id) {
            E e{};
            std::vector<std::uint64_t> tk;
            tk.reserve(n / 61 + 1);
            while (!go.load(std::memory_order_acquire)) cpu_relax();
            // Staggered: producer id starts id/P of an interval in.
            const std::uint64_t interval_ticks = static_cast<std::uint64_t>(c.interval_ns / ns_per_tick);
            std::uint64_t due_tsc = go_tsc + interval_ticks * static_cast<std::uint64_t>(id) / static_cast<std::uint64_t>(P);
            std::uint64_t due_ns = go_ns + c.interval_ns * static_cast<std::uint64_t>(id) / static_cast<std::uint64_t>(P);
            for (std::uint64_t i = 0; i < n; ++i) {
                if (paced && i % burst == 0) {
                    if (c.sleep) {
                        sleep_until_ns(due_ns);
                        due_ns += c.interval_ns;
                    } else {
                        while (rdtsc() < due_tsc) cpu_relax();
                        due_tsc += interval_ticks;
                    }
                }
                if (i == warm && started.fetch_add(1) == 0) t_start.store(now_ns(), std::memory_order_relaxed);
                e.seq = (static_cast<std::uint64_t>(id) << 56) | i;
                // The TSC is read only where it is used: for the stamp the
                // larger elements carry, and around the sampled pushes.
                const bool sample = i % 61 == 0;
                const std::uint64_t t0 = sizeof(E) != 8 || sample ? rdtsc_fenced() : 0;
                e.set_tsc(t0);
                q->push(e);
                if (sample) tk.push_back(rdtsc_fenced() - t0);
                if (!paced && (i + 1) % 1024 == 0) {
                    pushed.fetch_add(1024, std::memory_order_relaxed);
                    while (static_cast<std::int64_t>(pushed.load(std::memory_order_relaxed) -
                                                     popped.load(std::memory_order_relaxed)) > 8192)
                        cpu_relax();
                }
            }
            push_ticks[id] = std::move(tk);
        };

        std::vector<std::uint64_t> pop_tk, empty_tk, lat;
        pop_tk.reserve(total / 61 + 1);
        if (sizeof(E) != 8) lat.reserve(total);
        std::uint64_t polls = 0, empties = 0, t_end = 0;
        bool ok = true;
        auto consumer = [&] {
            E e{};
            std::vector<std::uint64_t> next(P, 0);
            std::uint64_t got = 0, mine = 0;
            while (got < total) {
                const bool sample = polls++ % 61 == 0;
                const std::uint64_t t0 = sample ? rdtsc_fenced() : 0;
                if (!q->try_pop(e)) {
                    if (sample) empty_tk.push_back(rdtsc_fenced() - t0);
                    ++empties;
                    if (mine) { popped.fetch_add(mine, std::memory_order_relaxed); mine = 0; }
                    cpu_relax();
                    continue;
                }
                const std::uint64_t t1 = sizeof(E) != 8 || sample ? rdtsc_fenced() : 0;
                if (sample) pop_tk.push_back(t1 - t0);
                const int p = static_cast<int>(e.seq >> 56);
                const std::uint64_t s = e.seq & ((1ull << 56) - 1);
                if (p >= P || s != next[p]) ok = false;  // each producer's order, exactly once
                else ++next[p];
                if (sizeof(E) != 8 && s >= warm) lat.push_back(t1 - e.tsc());
                ++got;
                if (++mine == 256) { popped.fetch_add(mine, std::memory_order_relaxed); mine = 0; }
            }
            t_end = now_ns();
        };

        std::vector<std::thread> ts;
        const int k = static_cast<int>(c.cpus.size());
        ts.emplace_back(consumer);
        pin_thread(ts.back(), c.cpus[0]);
        for (int p = 0; p < P; ++p) {
            ts.emplace_back(producer, p);
            pin_thread(ts.back(), c.cpus[1 + p % (k - 1)]);
        }
        go_tsc = rdtsc() + static_cast<std::uint64_t>(1e6 / ns_per_tick);  // a millisecond to settle
        go_ns = monotonic_ns() + 1'000'000;
        go.store(true, std::memory_order_release);
        for (auto& t : ts) t.join();
        if (!ok) { std::fprintf(stderr, "a producer's elements were lost, duplicated or reordered in %s\n", c.variant.c_str()); std::exit(1); }

        std::vector<std::uint64_t> push_all;
        for (auto& t : push_ticks) push_all.insert(push_all.end(), t.begin(), t.end());
        const std::uint64_t measured = total - warm * static_cast<std::uint64_t>(P);
        const double secs = static_cast<double>(t_end - t_start.load()) / 1e9;

        result_line r;
        r.set("workload", std::string("W13"));
        r.set("mode", std::string(paced ? "paced" : "stream"));
        r.set("variant", c.variant);
        r.set("elem", c.elem);
        r.set("producers", P);
        if (paced) {
            r.set("interval_ns", c.interval_ns);
            r.set("burst", burst);
            r.set("sleep", std::string(c.sleep ? "yes" : "no"));
            r.set("offered_mps", static_cast<double>(P) * static_cast<double>(burst) * 1e3 / static_cast<double>(c.interval_ns));
        }
        std::string cpu_list;
        for (int x : c.cpus) cpu_list += (cpu_list.empty() ? "" : ",") + std::to_string(x);
        r.set("cpus", cpu_list);
        r.set("rep", c.rep);
        r.set("n_total", total);
        r.set("mops", static_cast<double>(measured) / secs / 1e6);
        percentiles pp = compute_percentiles(push_all, ns_per_tick);
        r.set("push_p50", pp.p50); r.set("push_p99", pp.p99);
        percentiles po = compute_percentiles(pop_tk, ns_per_tick);
        r.set("pop_p50", po.p50); r.set("pop_p99", po.p99);
        percentiles pe = compute_percentiles(empty_tk, ns_per_tick);
        r.set("empty_p50", pe.p50); r.set("empty_p99", pe.p99);
        r.set("polls_per_elem", static_cast<double>(polls) / static_cast<double>(total));
        r.set("empty_polls", empties);
        if (!lat.empty()) {
            percentiles pl = compute_percentiles(lat, ns_per_tick);
            r.set("lat_p50", pl.p50); r.set("lat_p99", pl.p99); r.set("lat_p999", pl.p999); r.set("lat_max", pl.max);
        }
        r.set("tsc_hz", c.tsc_hz);
        r.write(c.json);
        std::fprintf(stderr, "W13 %-6s %-20s %3dP E%-3s %8.2f Mops/s push %.0f pop %.0f empty %.0f ns lat p50 %.0f p99 %.0f ns\n",
                     paced ? "paced" : "stream", c.variant.c_str(), P, c.elem.c_str(),
                     static_cast<double>(measured) / secs / 1e6, pp.p50, po.p50, pe.p50,
                     lat.empty() ? 0.0 : compute_percentiles(lat, ns_per_tick).p50,
                     lat.empty() ? 0.0 : compute_percentiles(lat, ns_per_tick).p99);
    }
};

} // namespace

int main(int argc, char** argv) {
    args a(argc, argv);
    config c;
    c.variant = a.get("variant", "shim_laned_move");
    c.elem = a.get("elem", "8");
    c.json = a.get("json", "");
    c.producers = static_cast<int>(a.getu("producers", 1));
    c.n = a.getu("n", 4'000'000 / static_cast<std::uint64_t>(c.producers < 1 ? 1 : c.producers));
    c.interval_ns = a.getu("interval-ns", 0);
    c.burst = a.getu("burst", 1);
    c.sleep = a.has("sleep");
    c.rep = static_cast<int>(a.getu("rep", 0));
    if (c.producers < 1 || c.producers > 255) { std::fprintf(stderr, "--producers is 1 to 255\n"); return 2; }
    const std::string cl = a.get("cpus", "");
    if (cl.empty()) {
        c.cpus = pick_distinct_cores(c.producers + 1);
    } else {
        std::size_t i = 0;
        while (i < cl.size()) { c.cpus.push_back(std::atoi(cl.c_str() + i)); while (i < cl.size() && cl[i] != ',') ++i; if (i < cl.size()) ++i; }
    }
    if (c.cpus.size() < 2) { std::fprintf(stderr, "--cpus needs the consumer's and at least one producer's\n"); return 2; }
    c.tsc_hz = calibrate_tsc_hz();
    bool ok = false;
    if (c.elem == "8") ok = with_mpsc_variant<E8>(c.variant, runner<E8>(c));
    else if (c.elem == "64") ok = with_mpsc_variant<E64>(c.variant, runner<E64>(c));
    else if (c.elem == "256") ok = with_mpsc_variant<E256>(c.variant, runner<E256>(c));
    else { std::fprintf(stderr, "--elem must be 8, 64 or 256\n"); return 2; }
    if (!ok) { std::fprintf(stderr, "unknown variant %s\n", c.variant.c_str()); return 2; }
    return 0;
}
