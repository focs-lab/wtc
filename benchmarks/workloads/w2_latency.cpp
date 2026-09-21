// W2: push-to-pop latency distribution.
//
//   w2_latency --variant shim_spsc --elem 64 --interval-ns 1000 --samples 1000000
//              [--pingpong] [--cpus 4,5 | --smt-siblings] [--json runs.jsonl]
//
// Open loop (default): the producer sends one element every interval-ns,
// busy-waiting on the timestamp counter between sends so that its own
// scheduling never bunches elements; each element carries the counter at
// send, the consumer records the difference at receive. Percentiles come
// from the consumer's own samples, warm-up discarded. The tail is the
// quantity this project is about, so the sample array is preallocated and
// nothing allocates on the measured path except the queue itself.
//
// --pingpong: two queues, one element in flight, the same thread sends and
// waits for the echo; reported latency is half the round trip. It does not
// depend on the two cores agreeing about the counter and serves as a
// cross-check of the open-loop numbers.

#include "adapters.hpp"
#include "elements.hpp"
#include "harness.hpp"

#include <thread>

using namespace wtc_bench;

namespace {

struct config {
    std::string variant, elem, wait, json;
    std::uint64_t samples, warm, interval_ns;
    std::vector<int> cpus;
    double tsc_hz;
    int rep;
    bool smt, pingpong;
};

template <class E>
struct runner {
    const config& c;
    explicit runner(const config& cfg) : c(cfg) {}

    void report(std::vector<std::uint64_t>& ticks, const char* mode, long f0, long f1) {
        const double ns_per_tick = 1e9 / c.tsc_hz;
        percentiles p = compute_percentiles(ticks, ns_per_tick);
        result_line r;
        r.set("workload", std::string("W2"));
        r.set("mode", std::string(mode));
        r.set("variant", c.variant);
        r.set("elem", c.elem);
        r.set("wait", c.wait);
        r.set("interval_ns", c.interval_ns);
        r.set("cpus", std::to_string(c.cpus[0]) + "," + std::to_string(c.cpus[1]));
        r.set("smt", c.smt ? 1 : 0);
        r.set("rep", c.rep);
        r.set("n_samples", static_cast<std::uint64_t>(ticks.size()));
        r.set("p50", p.p50); r.set("p90", p.p90); r.set("p99", p.p99);
        r.set("p999", p.p999); r.set("p9999", p.p9999); r.set("max_ns", p.max);
        r.set("freq_before_khz", static_cast<std::uint64_t>(f0));
        r.set("freq_after_khz", static_cast<std::uint64_t>(f1));
        r.set("tsc_hz", c.tsc_hz);
        r.write(c.json);
        std::fprintf(stderr, "W2 %-9s %-22s E%-4s @%5lluns  p50 %7.0f  p99 %7.0f  p99.9 %7.0f  p99.99 %8.0f  max %9.0f ns\n",
                     mode, c.variant.c_str(), c.elem.c_str(), static_cast<unsigned long long>(c.interval_ns),
                     p.p50, p.p99, p.p999, p.p9999, p.max);
    }

    template <class Q> void run_open_loop() {
        Q q;
        const std::uint64_t total = c.warm + c.samples;
        const std::uint64_t interval_ticks = static_cast<std::uint64_t>(c.interval_ns * c.tsc_hz / 1e9);
        std::vector<std::uint64_t> ticks; ticks.reserve(c.samples);
        std::atomic<bool> order_ok{true};

        std::thread prod([&] {
            E e{};
            std::uint64_t next = rdtsc();
            for (std::uint64_t i = 0; i < total; ++i) {
                while (rdtsc() < next) cpu_relax();
                e.seq = i; e.set_tsc(rdtsc());
                q.push(e);
                next += interval_ticks;
            }
        });
        std::thread cons([&] {
            E e{};
            unsigned misses = 0;
            for (std::uint64_t got = 0; got < total;) {
                if (!q.try_pop(e)) { wait_policy(c.wait, misses); continue; }
                const std::uint64_t t = rdtsc();
                if (e.seq != got) { order_ok.store(false); break; }
                if (got >= c.warm) ticks.push_back(t - e.tsc());
                ++got;
            }
        });
        pin_thread(prod, c.cpus[0]);
        pin_thread(cons, c.cpus[1]);
        const long f0 = cur_freq_khz(c.cpus[0]);
        prod.join(); cons.join();
        const long f1 = cur_freq_khz(c.cpus[0]);
        if (!order_ok.load()) { std::fprintf(stderr, "FIFO broken for %s\n", c.variant.c_str()); std::exit(1); }
        report(ticks, "open", f0, f1);
    }

    template <class Q> void run_pingpong() {
        Q there, back;
        const std::uint64_t total = c.warm + c.samples;
        std::vector<std::uint64_t> ticks; ticks.reserve(c.samples);
        std::thread echo([&] {
            E e{};
            unsigned misses = 0;
            for (std::uint64_t got = 0; got < total;) {
                if (!there.try_pop(e)) { wait_policy(c.wait, misses); continue; }
                back.push(e);
                ++got;
            }
        });
        std::thread ping([&] {
            E e{};
            unsigned misses = 0;
            for (std::uint64_t i = 0; i < total; ++i) {
                e.seq = i;
                const std::uint64_t t0 = rdtsc();
                there.push(e);
                E r{};
                while (!back.try_pop(r)) wait_policy(c.wait, misses);
                const std::uint64_t t1 = rdtsc();
                if (i >= c.warm) ticks.push_back((t1 - t0) / 2);
            }
        });
        pin_thread(ping, c.cpus[0]);
        pin_thread(echo, c.cpus[1]);
        const long f0 = cur_freq_khz(c.cpus[0]);
        ping.join(); echo.join();
        const long f1 = cur_freq_khz(c.cpus[0]);
        report(ticks, "pingpong", f0, f1);
    }

    template <class Q> void run() {
        if (c.pingpong) run_pingpong<Q>(); else run_open_loop<Q>();
    }
};

} // namespace

int main(int argc, char** argv) {
    args a(argc, argv);
    config c;
    c.variant = a.get("variant", "shim_generic");
    c.elem = a.get("elem", "64");
    c.wait = a.get("wait", "spin");
    c.json = a.get("json", "");
    c.samples = a.getu("samples", 1'000'000);
    c.warm = a.getu("warm", 100'000);
    c.interval_ns = a.getu("interval-ns", 1000);
    c.rep = static_cast<int>(a.getu("rep", 0));
    c.smt = a.has("smt-siblings");
    c.pingpong = a.has("pingpong");
    c.cpus = choose_cpus(a, 2);
    c.tsc_hz = calibrate_tsc_hz();

    bool ok = false;
    if (c.elem == "8") { std::fprintf(stderr, "E8 carries no timestamp; use 64 or 256\n"); return 2; }
    else if (c.elem == "64") ok = with_variant<E64>(c.variant, runner<E64>(c));
    else if (c.elem == "256") ok = with_variant<E256>(c.variant, runner<E256>(c));
    else { std::fprintf(stderr, "--elem must be 64 or 256\n"); return 2; }
    if (!ok) { std::fprintf(stderr, "unknown variant %s\n", c.variant.c_str()); return 2; }
    return 0;
}
