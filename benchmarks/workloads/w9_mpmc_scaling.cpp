// W9: a general-purpose queue under many producers and consumers.
//
//   w9_mpmc_scaling --variant moodycamel_cq_sp --mode split --producers 4 --consumers 4
//                   [--elem 8|64] [--n N] [--cpus ...] [--json runs.jsonl]
//   w9_mpmc_scaling --variant onetbb_cq_sp --mode pairwise --threads 8 [--n N] ...
//
// Two questions. Whether the shim's general-purpose queue, the one every
// unproved queue keeps, is a fair baseline next to the general-purpose
// queues a programmer would otherwise use, as the thread count grows; and
// whether keeping it costs anything where the compiler proves nothing.
//
// split: P producers push n elements each, C consumers pop until all P*n
// have arrived; each element carries its producer in its top byte and every
// consumer checks each producer's elements arrive in order. Producers wait
// while more than 8192 elements are in flight, as in W5, so an unbounded
// queue measures the queue and not the allocator. Throughput is elements
// delivered a second; with 64-byte elements, also each element's latency.
//
// pairwise: T threads each push one element and pop one, n times, the
// workload the lock-free queue literature reports (LCRQ, SCQ, wCQ).
// Throughput is operations a second, a push and a pop counting two. No
// order can be checked when a thread pops what another pushed, so the
// check is conservation: the popped values add up to the pushed ones.
//
// The single-producer queues run only at one producer and one consumer,
// the shim's list for many producers (shim_mpsc) only with one consumer,
// and its queue for many consumers (shim_spmc) only with one producer, in
// split mode.

#include "adapters.hpp"
#include "elements.hpp"
#include "harness.hpp"

#include <thread>

using namespace wtc_bench;

namespace {

struct config {
    std::string variant, mode, elem, wait, json;
    std::uint64_t n;
    int producers, consumers, threads;
    std::vector<int> cpus;
    double tsc_hz;
    int rep;
};

const char* const kSafe[] = {"shim_generic", "orig_generic", "std_mutex",
                             "moodycamel_cq_sp", "boost_ms_queue",
                             "lcrq", "lprq", "lscq", "faaarray",
#if defined(WTC_HAVE_TBB)
                             "onetbb_cq_sp",
#endif
};

template <class E>
struct runner {
    const config& c;
    explicit runner(const config& cfg) : c(cfg) {}

    void report(double mops, std::vector<std::uint64_t>& ticks, std::uint64_t n_ops, std::uint64_t n_total) {
        result_line r;
        r.set("workload", std::string("W9"));
        r.set("mode", c.mode);
        r.set("variant", c.variant);
        r.set("elem", c.elem);
        r.set("wait", c.wait);
        if (c.mode == "split") {
            r.set("producers", c.producers);
            r.set("consumers", c.consumers);
        } else {
            r.set("threads", c.threads);
        }
        std::string cpu_list;
        for (int x : c.cpus) cpu_list += (cpu_list.empty() ? "" : ",") + std::to_string(x);
        r.set("cpus", cpu_list);
        r.set("rep", c.rep);
        r.set("n_ops", n_ops);
        r.set("n_total", n_total);
        r.set("mops", mops);
        if (!ticks.empty()) {
            percentiles p = compute_percentiles(ticks, 1e9 / c.tsc_hz);
            r.set("p50", p.p50); r.set("p99", p.p99); r.set("p999", p.p999); r.set("max_ns", p.max);
            r.set("hist_ns", log_histogram(ticks, 1e9 / c.tsc_hz));
        }
        r.set("tsc_hz", c.tsc_hz);
        r.write(c.json);
        std::fprintf(stderr, "W9 %-8s %-20s %s %8.2f Mops/s\n", c.mode.c_str(), c.variant.c_str(),
                     c.mode == "split" ? (std::to_string(c.producers) + "P" + std::to_string(c.consumers) + "C").c_str()
                                       : (std::to_string(c.threads) + "T").c_str(),
                     mops);
    }

    template <class Q> void split() {
        Q q;
        const int P = c.producers, C = c.consumers;
        const std::uint64_t n = c.n, warm = n / 10, total = n * static_cast<std::uint64_t>(P);
        std::atomic<std::uint64_t> t_start{0}, t_end{0}, popped{0}, pushed{0};
        std::atomic<int> started{0};
        std::atomic<bool> ok{true};
        std::vector<std::vector<std::uint64_t>> ticks(C);
        for (auto& t : ticks) t.reserve(sizeof(E) == 8 ? 0 : total / C + 1);

        auto producer = [&](int id) {
            E e{};
            for (std::uint64_t i = 0; i < n; ++i) {
                if (i == warm && started.fetch_add(1) == 0) t_start.store(now_ns(), std::memory_order_relaxed);
                e.seq = (static_cast<std::uint64_t>(id) << 56) | i;
                e.set_tsc(rdtsc_fenced());
                q.push(e);
                if ((i + 1) % 1024 == 0) {
                    pushed.fetch_add(1024, std::memory_order_relaxed);
                    while (static_cast<std::int64_t>(pushed.load(std::memory_order_relaxed) -
                                                     popped.load(std::memory_order_relaxed)) > 8192)
                        cpu_relax();
                }
            }
        };
        // A consumer counts what it pops locally and adds the count to popped
        // every 256 elements and whenever the queue is empty, which is also
        // the only time it looks whether everything has arrived: one counter
        // that every consumer added to on every element was a contended
        // atomic of the benchmark's own, paid on every pop. Its samples go to
        // a vector of its own, handed over at the end; the vectors' headers
        // sit side by side in ticks.
        auto consumer = [&](int id) {
            E e{};
            unsigned misses = 0;
            std::uint64_t mine = 0;
            std::vector<std::uint64_t> last(P, 0);
            std::vector<std::uint64_t> tk = std::move(ticks[id]);
            for (;;) {
                if (!q.try_pop(e)) {
                    if (mine) { popped.fetch_add(mine, std::memory_order_relaxed); mine = 0; }
                    if (popped.load(std::memory_order_relaxed) >= total) break;
                    wait_policy(c.wait, misses);
                    continue;
                }
                const std::uint64_t t = rdtsc_fenced();
                const int p = static_cast<int>(e.seq >> 56);
                const std::uint64_t s = e.seq & ((1ull << 56) - 1);
                if (s < last[p]) ok.store(false);   // each producer's order must hold
                last[p] = s;
                if (sizeof(E) != 8 && s >= warm) tk.push_back(t - e.tsc());
                if (++mine == 256) { popped.fetch_add(mine, std::memory_order_relaxed); mine = 0; }
            }
            t_end.store(now_ns(), std::memory_order_relaxed);
            ticks[id] = std::move(tk);
        };
        std::vector<std::thread> ts;
        for (int p = 0; p < P; ++p) { ts.emplace_back(producer, p); pin_thread(ts.back(), c.cpus[p]); }
        for (int k = 0; k < C; ++k) { ts.emplace_back(consumer, k); pin_thread(ts.back(), c.cpus[P + k]); }
        for (auto& t : ts) t.join();
        if (!ok.load()) { std::fprintf(stderr, "per-producer order broken for %s\n", c.variant.c_str()); std::exit(1); }

        std::vector<std::uint64_t> all;
        for (auto& t : ticks) all.insert(all.end(), t.begin(), t.end());
        const std::uint64_t measured = total - warm * static_cast<std::uint64_t>(P);
        const double secs = static_cast<double>(t_end.load() - t_start.load()) / 1e9;
        report(static_cast<double>(measured) / secs / 1e6, all, measured, total);
    }

    template <class Q> void pairwise() {
        Q q;
        const int T = c.threads;
        const std::uint64_t n = c.n, warm = n / 10;
        std::atomic<std::uint64_t> t_start{0}, t_end{0}, popped_sum{0};
        std::atomic<int> started{0}, done{0};

        auto worker = [&](int id) {
            E e{}, out{};
            unsigned misses = 0;
            std::uint64_t sum = 0;
            for (std::uint64_t i = 0; i < n; ++i) {
                if (i == warm && started.fetch_add(1) == 0) t_start.store(now_ns(), std::memory_order_relaxed);
                e.seq = (static_cast<std::uint64_t>(id) << 56) | i;
                q.push(e);
                while (!q.try_pop(out)) wait_policy(c.wait, misses);
                sum += out.seq;
            }
            popped_sum.fetch_add(sum, std::memory_order_relaxed);
            if (done.fetch_add(1) + 1 == T) t_end.store(now_ns(), std::memory_order_relaxed);
        };
        std::vector<std::thread> ts;
        for (int k = 0; k < T; ++k) { ts.emplace_back(worker, k); pin_thread(ts.back(), c.cpus[k]); }
        for (auto& t : ts) t.join();

        std::uint64_t want = 0;
        for (int k = 0; k < T; ++k)
            want += static_cast<std::uint64_t>(k) * (n << 56) + n * (n - 1) / 2;
        if (popped_sum.load() != want) { std::fprintf(stderr, "elements lost or duplicated for %s\n", c.variant.c_str()); std::exit(1); }

        const std::uint64_t ops = 2 * (n - warm) * static_cast<std::uint64_t>(T);
        const double secs = static_cast<double>(t_end.load() - t_start.load()) / 1e9;
        std::vector<std::uint64_t> none;
        report(static_cast<double>(ops) / secs / 1e6, none, ops, n * static_cast<std::uint64_t>(T));
    }

    template <class Q> void run() {
        if (c.mode == "pairwise") pairwise<Q>(); else split<Q>();
    }
};

} // namespace

int main(int argc, char** argv) {
    args a(argc, argv);
    config c;
    c.variant = a.get("variant", "shim_generic");
    c.mode = a.get("mode", "split");
    c.elem = a.get("elem", "8");
    c.wait = a.get("wait", "spin");
    c.json = a.get("json", "");
    c.n = a.getu("n", 2'000'000);
    c.producers = static_cast<int>(a.getu("producers", 1));
    c.consumers = static_cast<int>(a.getu("consumers", 1));
    c.threads = static_cast<int>(a.getu("threads", 2));
    c.rep = static_cast<int>(a.getu("rep", 0));
    if (c.mode != "split" && c.mode != "pairwise") { std::fprintf(stderr, "--mode is split or pairwise\n"); return 2; }
    if (c.producers < 1 || c.consumers < 1 || c.threads < 1 || c.producers > 255) { std::fprintf(stderr, "bad thread counts\n"); return 2; }
    const int need = c.mode == "split" ? c.producers + c.consumers : c.threads;
    c.cpus = choose_cpus(a, need);
    c.tsc_hz = calibrate_tsc_hz();

    bool safe = false;
    for (auto s : kSafe) if (c.variant == s) safe = true;
    const bool one_to_one = c.mode == "split" && c.producers == 1 && c.consumers == 1;
    if (c.variant == "shim_mpsc" || c.variant == "orig_mpsc") {
        if (c.mode != "split" || c.consumers != 1) {
            std::fprintf(stderr, "%s has one consumer: only --mode split with --consumers 1\n", c.variant.c_str());
            return 2;
        }
        safe = true;
    }
    if (c.variant == "shim_spmc") {
        if (c.mode != "split" || c.producers != 1) {
            std::fprintf(stderr, "shim_spmc has one producer: only --mode split with --producers 1\n");
            return 2;
        }
        safe = true;
    }
    if (!safe && !one_to_one) {
        std::fprintf(stderr, "%s is a single-producer queue: only --mode split with one producer and one consumer\n", c.variant.c_str());
        return 2;
    }
    bool ok = false;
    if (c.elem == "8") ok = with_variant<E8>(c.variant, runner<E8>(c));
    else if (c.elem == "64") ok = with_variant<E64>(c.variant, runner<E64>(c));
    else { std::fprintf(stderr, "--elem must be 8 or 64\n"); return 2; }
    if (!ok) { std::fprintf(stderr, "unknown variant %s\n", c.variant.c_str()); return 2; }
    return 0;
}
