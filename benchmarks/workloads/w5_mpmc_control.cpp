// W5: the fallback control. Two producers, two consumers, one queue.
//
//   w5_mpmc_control --variant shim_generic --n 8000000 [--cpus a,b,c,d]
//                   [--force-unsafe] [--json runs.jsonl]
//
// The claim this carries: where the verdict is not SPSC, the compiler leaves
// the generic implementation, and the generic implementation with the tag
// costs the same as the original file. So the rows that matter are
// shim_generic against orig_generic, with the library MPMC queues for
// context.
//
// Only MPMC-safe variants are accepted, unless --force-unsafe is given: that
// is the negative control, running the SPSC ring under two producers so that
// WTC_CHECK proves it can fire. Without WTC_CHECK the negative control is
// undefined behaviour and this program says so and refuses.

#include "adapters.hpp"
#include "elements.hpp"
#include "harness.hpp"

#include <thread>

using namespace wtc_bench;

namespace {

struct config {
    std::string variant, wait, json;
    std::uint64_t n;
    std::vector<int> cpus;
    double tsc_hz;
    int rep;
    bool force;
};

const char* const kSafe[] = {"shim_generic", "orig_generic", "std_mutex",
                             "moodycamel_cq_sp", "boost_ms_queue",
#if defined(WTC_HAVE_TBB)
                             "onetbb_cq_sp",
#endif
};

struct runner {
    const config& c;
    explicit runner(const config& cfg) : c(cfg) {}

    template <class Q> void run() {
        Q q;
        const std::uint64_t n = c.n;              // per producer
        const std::uint64_t warm = n / 10;
        std::atomic<std::uint64_t> t_start{0}, t_end{0};
        std::atomic<std::uint64_t> popped{0};
        // Both producers add to one pushed counter every 1024 pushes and wait
        // while more than 8192 are in flight. The difference is taken signed:
        // a producer's own count is no estimate of the total, and an unsigned
        // subtraction of popped from it wrapped and spun forever, measured.
        std::atomic<std::uint64_t> pushed{0};
        std::atomic<bool> ok{true};
        std::vector<std::uint64_t> ticks[2];
        ticks[0].reserve(n); ticks[1].reserve(n);
        std::atomic<int> started{0};

        auto producer = [&](int id) {
            E64 e{};
            for (std::uint64_t i = 0; i < n; ++i) {
                if (i == warm && started.fetch_add(1) == 0) t_start.store(now_ns(), std::memory_order_relaxed);
                e.seq = (static_cast<std::uint64_t>(id) << 56) | i;
                e.set_tsc(rdtsc());
                q.push(e);
                if ((i + 1) % 1024 == 0) {
                    pushed.fetch_add(1024, std::memory_order_relaxed);
                    while (static_cast<std::int64_t>(pushed.load(std::memory_order_relaxed) - popped.load(std::memory_order_relaxed)) > 8192) cpu_relax();
                }
            }
        };
        auto consumer = [&](int id) {
            E64 e{};
            unsigned misses = 0;
            std::uint64_t last[2] = {0, 0};
            for (;;) {
                if (popped.load(std::memory_order_relaxed) >= 2 * n) break;
                if (!q.try_pop(e)) { wait_policy(c.wait, misses); continue; }
                const std::uint64_t t = rdtsc();
                const int p = static_cast<int>(e.seq >> 56);
                const std::uint64_t s = e.seq & ((1ull << 56) - 1);
                if (s < last[p]) ok.store(false);   // per-producer order must hold
                last[p] = s;
                if (s >= warm) ticks[id].push_back(t - e.tsc());
                popped.fetch_add(1, std::memory_order_relaxed);
            }
            t_end.store(now_ns(), std::memory_order_relaxed);
        };
        std::thread p0(producer, 0), p1(producer, 1), c0(consumer, 0), c1(consumer, 1);
        pin_thread(p0, c.cpus[0]); pin_thread(p1, c.cpus[1]); pin_thread(c0, c.cpus[2]); pin_thread(c1, c.cpus[3]);
        p0.join(); p1.join(); c0.join(); c1.join();
        if (!ok.load()) { std::fprintf(stderr, "per-producer order broken for %s\n", c.variant.c_str()); std::exit(1); }

        std::vector<std::uint64_t> all(ticks[0]); all.insert(all.end(), ticks[1].begin(), ticks[1].end());
        const double secs = static_cast<double>(t_end.load() - t_start.load()) / 1e9;
        const double mops = static_cast<double>(all.size()) / secs / 1e6;
        percentiles p = compute_percentiles(all, 1e9 / c.tsc_hz);

        result_line r;
        r.set("workload", std::string("W5"));
        r.set("variant", c.variant);
        r.set("elem", std::string("64"));
        r.set("wait", c.wait);
        r.set("cpus", std::to_string(c.cpus[0]) + "," + std::to_string(c.cpus[1]) + "," + std::to_string(c.cpus[2]) + "," + std::to_string(c.cpus[3]));
        r.set("rep", c.rep);
        r.set("n_ops", static_cast<std::uint64_t>(all.size()));
        r.set("mops", mops);
        r.set("p50", p.p50); r.set("p99", p.p99); r.set("p999", p.p999); r.set("max_ns", p.max);
        r.set("tsc_hz", c.tsc_hz);
        r.write(c.json);
        std::fprintf(stderr, "W5 2P2C %-22s %7.2f Mops/s  p50 %6.0f  p99 %7.0f  p99.9 %7.0f ns\n",
                     c.variant.c_str(), mops, p.p50, p.p99, p.p999);
    }
};

} // namespace

int main(int argc, char** argv) {
    args a(argc, argv);
    config c;
    c.variant = a.get("variant", "shim_generic");
    c.wait = a.get("wait", "spin");
    c.json = a.get("json", "");
    c.n = a.getu("n", 4'000'000);
    c.rep = static_cast<int>(a.getu("rep", 0));
    c.force = a.has("force-unsafe");
    c.cpus = choose_cpus(a, 4);
    c.tsc_hz = calibrate_tsc_hz();

    bool safe = false;
    for (auto s : kSafe) if (c.variant == s) safe = true;
    if (!safe && !c.force) {
        std::fprintf(stderr, "%s is not MPMC-safe; pass --force-unsafe only for the WTC_CHECK negative control\n", c.variant.c_str());
        return 2;
    }
#ifndef WTC_CHECK
    if (!safe) { std::fprintf(stderr, "refusing to run an unsafe variant without WTC_CHECK: that would be undefined behaviour, not a test\n"); return 2; }
#endif
    if (!with_variant<E64>(c.variant, runner(c))) { std::fprintf(stderr, "unknown variant %s\n", c.variant.c_str()); return 2; }
    return 0;
}
