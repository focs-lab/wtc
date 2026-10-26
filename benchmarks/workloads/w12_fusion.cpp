// W12: three pipeline stages on three threads, or fused on one.
//
//   w12_fusion --variant pipeline --work 20 [--rounds R] [--n N]
//              [--cpus 16,17,18] [--rep 0] [--json runs.jsonl]
//   w12_fusion --calibrate      prints rounds of the work per ns
//
// Each record (64 bytes) passes three stages of --work ns of dependent
// arithmetic each (--rounds of it exactly, when given). "pipeline" runs the
// stages on three threads joined by the runtime's SPSC rings, the records
// in the cells, which is what the compiler substitutes for a pipeline whose
// stages have one thread each; "fused" runs the same three stages one after
// another in one loop on the first CPU. Below some grain the fused loop
// wins, because a record's passage between cores costs more than its work;
// the sweep over --work finds where. What it measures is throughput: the
// pipeline spends three cores on it and the fused loop one, and a record's
// latency through either is not measured.
//
// Each run lasts about a second (the record count depends on the variant
// and the grain unless --n is given), the first tenth of it warm-up. The
// last stage checks the sequence numbers and goes on draining after a
// mismatch, so a reorder fails the run rather than hanging it. Both
// variants print the same checksum of the first million records' results
// for the same --rounds, as "checksum <variant> <work> <rounds> <value>"
// on stdout, so runs of the two can be checked against each other. The
// time runs from the last stage's first record after the warm-up. Records
// in flight between the first stage and the last are bounded as in W1.

#include "harness.hpp"
#include "spsc_ring.hpp"   // runtime, via -I runtime

#include <thread>

using namespace wtc_bench;

namespace {

constexpr std::uint64_t kChecked = 1'000'000;   // records in the checksum

struct rec { std::uint64_t seq, v; std::uint64_t pad[6]; };

struct config {
    std::string variant, json;
    std::uint64_t n, warm, work_ns, rounds;
    std::vector<int> cpus;
    int rep;
};

struct outcome { std::uint64_t t_start, t_end, checksum; bool order_ok; };

outcome fused(const config& c) {
    pin_self(c.cpus[0]);
    outcome o{0, 0, 0, true};
    for (std::uint64_t i = 0; i < c.n; ++i) {
        if (i == c.warm) o.t_start = now_ns();
        rec r{i, i, {}};
        r.v = work_rounds(r.v, c.rounds);
        r.v = work_rounds(r.v, c.rounds);
        r.v = work_rounds(r.v, c.rounds);
        if (i < kChecked) o.checksum += r.v;
    }
    o.t_end = now_ns();
    return o;
}

outcome pipeline(const config& c) {
    wtc::rt::spsc_ring<rec> q1, q2;
    throttle th;
    std::atomic<std::uint64_t> t_start{0};
    outcome o{0, 0, 0, true};
    std::thread s1([&] {
        for (std::uint64_t i = 0; i < c.n; ++i) {
            rec r{i, i, {}};
            r.v = work_rounds(r.v, c.rounds);
            q1.push(r);
            th.producer_wait(i + 1);
        }
    });
    std::thread s2([&] {
        rec r;
        for (std::uint64_t i = 0; i < c.n; ++i) {
            while (!q1.try_pop(r)) cpu_relax();
            r.v = work_rounds(r.v, c.rounds);
            q2.push(r);
        }
    });
    std::thread s3([&] {
        rec r;
        for (std::uint64_t i = 0; i < c.n; ++i) {
            while (!q2.try_pop(r)) cpu_relax();
            // The window opens as the last stage takes the first record
            // after the warm-up, so it counts exactly the records it times.
            if (i == c.warm) t_start.store(now_ns(), std::memory_order_relaxed);
            if (r.seq != i) o.order_ok = false;
            r.v = work_rounds(r.v, c.rounds);
            if (i < kChecked) o.checksum += r.v;
            th.consumer_progress(i + 1);
        }
        o.t_end = now_ns();
    });
    pin_thread(s1, c.cpus[0]);
    pin_thread(s2, c.cpus[1]);
    pin_thread(s3, c.cpus[2]);
    s1.join(); s2.join(); s3.join();
    o.t_start = t_start.load();
    return o;
}

} // namespace

int main(int argc, char** argv) {
    args a(argc, argv);
    if (a.has("calibrate")) {
        pin_self(pick_distinct_cores(1)[0]);
        std::printf("%.6f\n", work_rounds_per_ns());
        return 0;
    }
    config c;
    c.variant = a.get("variant", "pipeline");
    c.json = a.get("json", "");
    c.work_ns = a.getu("work", 0);
    c.rep = static_cast<int>(a.getu("rep", 0));
    if (c.variant != "pipeline" && c.variant != "fused") {
        std::fprintf(stderr, "unknown variant %s; known: fused pipeline\n", c.variant.c_str());
        return 2;
    }
    // About a second: a record costs the fused loop three stages' work and
    // the pipeline its slowest stage plus a passage of some 20 ns.
    const double est_ns = c.variant == "fused" ? 3.0 * c.work_ns + 1.0 : c.work_ns + 20.0;
    c.n = a.getu("n", std::clamp<std::uint64_t>(static_cast<std::uint64_t>(1e9 / est_ns),
                                                 2 * kChecked, 1'500'000'000));
    c.warm = a.getu("warm", c.n / 10);
    c.cpus = choose_cpus(a, 3);
    const double tsc_hz = calibrate_tsc_hz();
    // --rounds fixes the work exactly, so that variants run in separate
    // processes do the same arithmetic; a leg calibrates once and passes
    // it. Without it, --work is converted with this process's calibration.
    c.rounds = a.has("rounds") ? a.getu("rounds", 0)
                               : static_cast<std::uint64_t>(static_cast<double>(c.work_ns) * work_rounds_per_ns() + 0.5);

    const long f0 = cur_freq_khz(c.cpus[0]);
    const outcome o = c.variant == "fused" ? fused(c) : pipeline(c);
    const long f1 = cur_freq_khz(c.cpus[0]);
    if (!o.order_ok) { std::fprintf(stderr, "FIFO broken for %s\n", c.variant.c_str()); return 1; }

    const double secs = static_cast<double>(o.t_end - o.t_start) / 1e9;
    const double mops = static_cast<double>(c.n - c.warm) / secs / 1e6;
    result_line r;
    r.set("workload", std::string("W12"));
    r.set("variant", c.variant);
    r.set("work", c.work_ns);
    r.set("rounds", c.rounds);
    r.set("cpus", std::to_string(c.cpus[0]) + "," + std::to_string(c.cpus[1]) + "," + std::to_string(c.cpus[2]));
    r.set("rep", c.rep);
    r.set("n_ops", c.n - c.warm);
    r.set("n_total", c.n);
    r.set("wall_ns", o.t_end - o.t_start);
    r.set("mops", mops);
    r.set("ns_per_op", secs * 1e9 / static_cast<double>(c.n - c.warm));
    r.set("checksum", o.checksum);
    r.set("freq_before_khz", static_cast<std::uint64_t>(f0));
    r.set("freq_after_khz", static_cast<std::uint64_t>(f1));
    r.set("tsc_hz", tsc_hz);
    r.write(c.json);
    std::printf("checksum %s %llu %llu %llu\n", c.variant.c_str(), (unsigned long long)c.work_ns,
                (unsigned long long)c.rounds, (unsigned long long)o.checksum);
    std::fprintf(stderr, "W12 %-8s work %4llu ns  %9.2f Mrec/s\n", c.variant.c_str(),
                 (unsigned long long)c.work_ns, mops);
    return 0;
}
