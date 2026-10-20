// W3: an application-shaped pipeline. Three stages, two queues.
//
//   w3_pipeline --variant shim_spsc --n 10000000 [--rate 2000000] [--work 4]
//               [--cpus a,b,c] [--json runs.jsonl]
//
//   generate  -> q1 ->  transform  -> q2 ->  sink
//
// generate produces 64-byte records from a seeded generator, either as fast
// as it can (closed loop, the default) or one every 1/rate seconds by
// busy-waiting on the TSC (open loop). In the open loop a record's latency
// runs from the time it was due, so a generator held back by a full
// pipeline charges the delay to the records it held up; a run that did not
// sustain the offered rate is marked saturated. transform does a fixed amount of
// arithmetic on each record (--work rounds of a mixing function, about 15
// ns each) and forwards it. sink accumulates a checksum, which is the output
// oracle: every build flavour of this program must print the same number,
// and records the end-to-end latency of each record after warm-up.
//
// This is the number the gate quotes: not a queue in isolation but a queue
// doing the job queues do, between stages that also have work of their own.

#include "adapters.hpp"
#include "elements.hpp"
#include "harness.hpp"

#include <thread>

using namespace wtc_bench;

namespace {

struct config {
    std::string variant, wait, json;
    std::uint64_t n, warm, rate, work;
    std::vector<int> cpus;
    double tsc_hz;
    int rep;
};

inline std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 31; x *= 0x9E3779B97F4A7C15ull; x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 32;
    return x;
}

struct runner {
    const config& c;
    explicit runner(const config& cfg) : c(cfg) {}

    template <class Q> void run() {
        Q q1, q2;
        throttle th1, th2;
        const std::uint64_t n = c.n, warm = c.warm;
        std::vector<std::uint64_t> ticks; ticks.reserve(n - warm);
        std::atomic<std::uint64_t> t_start{0}, t_end{0};
        std::atomic<std::uint64_t> checksum{0};
        std::atomic<bool> ok{true};
        const std::uint64_t interval_ticks = c.rate ? static_cast<std::uint64_t>(c.tsc_hz / c.rate) : 0;

        std::thread gen([&] {
            E64 e{};
            std::uint64_t s = 0x1234567ull;
            std::uint64_t next = rdtsc();
            for (std::uint64_t i = 0; i < n; ++i) {
                std::uint64_t due = 0;
                if (interval_ticks) { while (rdtsc() < next) cpu_relax(); due = next; next += interval_ticks; }
                if (i == warm) t_start.store(now_ns(), std::memory_order_relaxed);
                e.seq = i;
                for (auto& p : e.pad) { s = mix(s + i); p = s; }
                e.set_tsc(interval_ticks ? due : rdtsc_fenced());
                q1.push(e);
                th1.producer_wait(i + 1);
            }
        });
        std::thread xform([&] {
            E64 e{};
            unsigned misses = 0;
            for (std::uint64_t got = 0; got < n;) {
                if (!q1.try_pop(e)) { wait_policy(c.wait, misses); continue; }
                th1.consumer_progress(++got);
                std::uint64_t x = e.seq;
                for (std::uint64_t r = 0; r < c.work; ++r)
                    for (auto p : e.pad) x = mix(x ^ p);
                e.pad[0] = x;
                q2.push(e);
                th2.producer_wait(got);
            }
        });
        std::thread sink([&] {
            E64 e{};
            unsigned misses = 0;
            std::uint64_t sum = 0;
            for (std::uint64_t got = 0; got < n;) {
                if (!q2.try_pop(e)) { wait_policy(c.wait, misses); continue; }
                const std::uint64_t t = rdtsc_fenced();
                if (e.seq != got) { ok.store(false); break; }
                sum += e.pad[0];
                if (got >= warm) ticks.push_back(t - e.tsc());
                th2.consumer_progress(++got);
            }
            t_end.store(now_ns(), std::memory_order_relaxed);
            checksum.store(sum);
        });
        pin_thread(gen, c.cpus[0]); pin_thread(xform, c.cpus[1]); pin_thread(sink, c.cpus[2]);
        const long f0 = cur_freq_khz(c.cpus[0]);
        gen.join(); xform.join(); sink.join();
        const long f1 = cur_freq_khz(c.cpus[0]);
        if (!ok.load()) { std::fprintf(stderr, "FIFO broken for %s\n", c.variant.c_str()); std::exit(1); }

        const double secs = static_cast<double>(t_end.load() - t_start.load()) / 1e9;
        const double mrec = static_cast<double>(n - warm) / secs / 1e6;
        percentiles p = compute_percentiles(ticks, 1e9 / c.tsc_hz);

        result_line r;
        r.set("workload", std::string("W3"));
        r.set("mode", c.rate ? std::string("open") : std::string("closed"));
        r.set("variant", c.variant);
        r.set("elem", std::string("64"));
        r.set("wait", c.wait);
        r.set("rate", c.rate);
        r.set("work", c.work);
        r.set("cpus", std::to_string(c.cpus[0]) + "," + std::to_string(c.cpus[1]) + "," + std::to_string(c.cpus[2]));
        r.set("rep", c.rep);
        r.set("n_ops", n - warm);
        r.set("mops", mrec);
        r.set("p50", p.p50); r.set("p90", p.p90); r.set("p99", p.p99);
        r.set("p999", p.p999); r.set("p9999", p.p9999); r.set("max_ns", p.max);
        r.set("checksum", checksum.load());
        r.set("hist_ns", log_histogram(ticks, 1e9 / c.tsc_hz));
        if (c.rate) r.set("saturated", mrec * 1e6 < 0.95 * static_cast<double>(c.rate) ? 1 : 0);
        r.set("freq_before_khz", static_cast<std::uint64_t>(f0));
        r.set("freq_after_khz", static_cast<std::uint64_t>(f1));
        r.set("tsc_hz", c.tsc_hz);
        r.write(c.json);
        std::printf("checksum %llu\n", static_cast<unsigned long long>(checksum.load()));
        std::fprintf(stderr, "W3 %-7s %-22s %7.2f Mrec/s  p50 %6.0f  p99 %7.0f  p99.9 %7.0f ns\n",
                     c.rate ? "open" : "closed", c.variant.c_str(), mrec, p.p50, p.p99, p.p999);
    }
};

} // namespace

int main(int argc, char** argv) {
    args a(argc, argv);
    config c;
    c.variant = a.get("variant", "shim_generic");
    c.wait = a.get("wait", "spin");
    c.json = a.get("json", "");
    c.n = a.getu("n", 10'000'000);
    c.warm = a.getu("warm", c.n / 10);
    c.rate = a.getu("rate", 0);
    c.work = a.getu("work", 4);
    c.rep = static_cast<int>(a.getu("rep", 0));
    c.cpus = choose_cpus(a, 3);
    c.tsc_hz = calibrate_tsc_hz();
    if (!with_variant<E64>(c.variant, runner(c))) { std::fprintf(stderr, "unknown variant %s\n", c.variant.c_str()); return 2; }
    return 0;
}
