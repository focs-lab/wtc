// W2: push-to-pop latency distribution.
//
//   w2_latency --variant shim_spsc --elem 64 --interval-ns 1000 --samples 1000000
//              [--pingpong] [--cpus 4,5 | --smt-siblings] [--json runs.jsonl]
//
// Open loop (default): the producer sends one element every interval-ns,
// busy-waiting on the timestamp counter between sends so that its own
// scheduling never bunches elements. Each element carries the time it was
// due and the time it was sent; the consumer records both differences at
// receive. Latency is reported from the due time, so a producer that falls
// behind its schedule -- because a push took long, or the queue is
// saturated -- charges the delay to the elements it held up instead of
// hiding it (coordinated omission); the svc_ percentiles, from the send
// time, are the queue's own share. The line also says at what rate the
// producer sent and the consumer drained, and whether the run kept up with
// the rate offered: a saturated run measures a backlog, not the queue. Both
// sides count, because the queue is unbounded: a producer always keeps its
// schedule, and a consumer that cannot keep up shows only in its own rate.
// Percentiles come from the consumer's own samples, warm-up discarded; the
// sample arrays are preallocated and nothing allocates on the measured path
// except the queue.
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

    struct schedule {
        double achieved_rate = 0;    // elements a second sent, over the measured part
        double drained_rate = 0;     // elements a second received, over the same
        double lag_max_ns = 0;       // how far behind its schedule the producer fell
        double late_frac = 0;        // sends more than one interval behind
    };

    void report(std::vector<std::uint64_t>& ticks, std::vector<std::uint64_t>& svc,
                const schedule& sch, const char* mode, long f0, long f1) {
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
        r.set("n_total", c.warm + c.samples);
        r.set("p50", p.p50); r.set("p90", p.p90); r.set("p99", p.p99);
        r.set("p999", p.p999); r.set("p9999", p.p9999); r.set("max_ns", p.max);
        r.set("freq_before_khz", static_cast<std::uint64_t>(f0));
        r.set("freq_after_khz", static_cast<std::uint64_t>(f1));
        r.set("tsc_hz", c.tsc_hz);
        r.set("hist_ns", log_histogram(ticks, ns_per_tick));
        if (!svc.empty()) {
            percentiles s = compute_percentiles(svc, ns_per_tick);
            const double offered = 1e9 / static_cast<double>(c.interval_ns);
            r.set("svc_p50", s.p50); r.set("svc_p99", s.p99); r.set("svc_p999", s.p999);
            r.set("offered_rate", offered);
            r.set("achieved_rate", sch.achieved_rate);
            r.set("drained_rate", sch.drained_rate);
            r.set("saturated", std::min(sch.achieved_rate, sch.drained_rate) < 0.95 * offered ? 1 : 0);
            r.set("lag_max_ns", sch.lag_max_ns);
            r.set("late_frac", sch.late_frac);
        }
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
        std::vector<std::uint64_t> svc; svc.reserve(c.samples);
        std::atomic<bool> order_ok{true};
        std::uint64_t first_sent = 0, last_sent = 0, lag_max = 0, late = 0;
        std::uint64_t first_recv = 0, last_recv = 0;

        std::thread prod([&] {
            E e{};
            std::uint64_t next = rdtsc();
            for (std::uint64_t i = 0; i < total; ++i) {
                while (rdtsc() < next) cpu_relax();
                const std::uint64_t now = rdtsc_fenced();
                if (i >= c.warm) {
                    const std::uint64_t lag = now - next;
                    if (lag > lag_max) lag_max = lag;
                    if (lag > interval_ticks) ++late;
                    if (i == c.warm) first_sent = now;
                    last_sent = now;
                }
                e.seq = i; e.set_tsc(next); e.set_sent(now);
                q.push(e);
                next += interval_ticks;
            }
        });
        std::thread cons([&] {
            E e{};
            unsigned misses = 0;
            for (std::uint64_t got = 0; got < total;) {
                if (!q.try_pop(e)) { wait_policy(c.wait, misses); continue; }
                const std::uint64_t t = rdtsc_fenced();
                if (e.seq != got) { order_ok.store(false); break; }
                if (got >= c.warm) {
                    ticks.push_back(t - e.tsc()); svc.push_back(t - e.sent());
                    if (got == c.warm) first_recv = t;
                    last_recv = t;
                }
                ++got;
            }
        });
        pin_thread(prod, c.cpus[0]);
        pin_thread(cons, c.cpus[1]);
        const long f0 = cur_freq_khz(c.cpus[0]);
        prod.join(); cons.join();
        const long f1 = cur_freq_khz(c.cpus[0]);
        if (!order_ok.load()) { std::fprintf(stderr, "FIFO broken for %s\n", c.variant.c_str()); std::exit(1); }
        schedule sch;
        const double ns_per_tick = 1e9 / c.tsc_hz;
        if (last_sent > first_sent)
            sch.achieved_rate = static_cast<double>(c.samples - 1) / (static_cast<double>(last_sent - first_sent) * ns_per_tick / 1e9);
        if (last_recv > first_recv)
            sch.drained_rate = static_cast<double>(c.samples - 1) / (static_cast<double>(last_recv - first_recv) * ns_per_tick / 1e9);
        sch.lag_max_ns = static_cast<double>(lag_max) * ns_per_tick;
        sch.late_frac = static_cast<double>(late) / static_cast<double>(c.samples);
        report(ticks, svc, sch, "open", f0, f1);
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
                const std::uint64_t t0 = rdtsc_fenced();
                there.push(e);
                E r{};
                while (!back.try_pop(r)) wait_policy(c.wait, misses);
                const std::uint64_t t1 = rdtsc_fenced();
                if (i >= c.warm) ticks.push_back((t1 - t0) / 2);
            }
        });
        pin_thread(ping, c.cpus[0]);
        pin_thread(echo, c.cpus[1]);
        const long f0 = cur_freq_khz(c.cpus[0]);
        ping.join(); echo.join();
        const long f1 = cur_freq_khz(c.cpus[0]);
        std::vector<std::uint64_t> none;
        report(ticks, none, schedule{}, "pingpong", f0, f1);
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
