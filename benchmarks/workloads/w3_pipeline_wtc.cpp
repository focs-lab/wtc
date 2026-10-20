// W3 again, but as a program the compiler can reason about: the queues are
// used directly, not through a benchmark adapter.
//
// This file exists because the adapter harness cannot answer the question
// this workload is for. Every variant in adapters.hpp holds its queue as a
// struct member, and a queue reached through a member is exactly what the
// analysis refuses to identify, so it says unknown and the fallback stays.
// That refusal is correct; it just means the adapter shape is the wrong
// shape for measuring the compiler's own decision. Here the queues are two
// globals, which is what an application-shaped program looks like and what
// the analysis can follow.
//
// Four builds from this one source:
//
//   generic   tools/wtc-cc --no-specialize        tag stays generic
//   hand      tools/wtc-cc --no-specialize -DWTC_HAND   source sets the tag
//   hand-inline  tools/wtc-cc --no-specialize -DWTC_HAND_INLINE  spsc_inline
//   compiler  tools/wtc-cc                        the verdicts set the tag
//
// The claim is that compiler is indistinguishable from hand-inline, and the
// checksum is identical in all four.
//
// The shape is the adapter pipeline's (w3_pipeline.cpp), so the two can be
// read side by side: the same in-flight bound, and in the open loop latency
// from each record's due time, with a run that did not sustain the offered
// rate marked saturated. Without the bound the generic build's closed loop
// measured a backlog of seconds, not the queue.
//
//   w3_pipeline_wtc [--n N] [--rate R] [--work W] [--cpus a,b,c] [--json F]

#include "queue.hpp"
#include "thread.hpp"

#include "elements.hpp"
#include "harness.hpp"

#include <thread>

using namespace wtc_bench;

namespace {

inline std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 31; x *= 0x9E3779B97F4A7C15ull; x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 32;
    return x;
}

// Two stage queues. Globals, and constructed once, which is the case the
// specialisation pass can rewrite.
#if defined(WTC_HAND_INLINE)
wtc::queue<E64> stage1(wtc::topology::spsc_inline);
wtc::queue<E64> stage2(wtc::topology::spsc_inline);
#elif defined(WTC_HAND)
wtc::queue<E64> stage1(wtc::topology::spsc);
wtc::queue<E64> stage2(wtc::topology::spsc);
#else
wtc::queue<E64> stage1;
wtc::queue<E64> stage2;
#endif

std::uint64_t g_n = 10'000'000, g_warm = 1'000'000, g_rate = 0, g_work = 4;
double g_tsc_hz = 0;
std::uint64_t g_t_start = 0, g_t_end = 0, g_checksum = 0;
bool g_order_ok = true;
std::vector<std::uint64_t> g_ticks;
throttle g_th1, g_th2;

void generate() {
    E64 e{};
    std::uint64_t s = 0x1234567ull;
    const std::uint64_t interval = g_rate ? static_cast<std::uint64_t>(g_tsc_hz / g_rate) : 0;
    std::uint64_t next = rdtsc();
    for (std::uint64_t i = 0; i < g_n; ++i) {
        std::uint64_t due = 0;
        if (interval) { while (rdtsc() < next) cpu_relax(); due = next; next += interval; }
        if (i == g_warm) g_t_start = now_ns();
        e.seq = i;
        for (auto& p : e.pad) { s = mix(s + i); p = s; }
        e.set_tsc(interval ? due : rdtsc_fenced());
        stage1.push(e);
        g_th1.producer_wait(i + 1);
    }
}

void transform() {
    for (std::uint64_t got = 0; got < g_n;) {
        auto p = stage1.try_pop();
        if (!p) { cpu_relax(); continue; }
        E64 e = *p;
        g_th1.consumer_progress(++got);
        std::uint64_t x = e.seq;
        for (std::uint64_t r = 0; r < g_work; ++r)
            for (auto v : e.pad) x = mix(x ^ v);
        e.pad[0] = x;
        stage2.push(e);
        g_th2.producer_wait(got);
    }
}

void sink() {
    std::uint64_t sum = 0;
    for (std::uint64_t got = 0; got < g_n;) {
        auto p = stage2.try_pop();
        if (!p) { cpu_relax(); continue; }
        const std::uint64_t t = rdtsc_fenced();
        if (p->seq != got) { g_order_ok = false; break; }
        sum += p->pad[0];
        if (got >= g_warm) g_ticks.push_back(t - p->tsc());
        g_th2.consumer_progress(++got);
    }
    g_t_end = now_ns();
    g_checksum = sum;
}

} // namespace

int main(int argc, char** argv) {
    args a(argc, argv);
    g_n = a.getu("n", 10'000'000);
    g_warm = a.getu("warm", g_n / 10);
    g_rate = a.getu("rate", 0);
    g_work = a.getu("work", 4);
    const int rep = static_cast<int>(a.getu("rep", 0));
    const std::string json = a.get("json", "");
    const std::string flavour = a.get("flavour", "unknown");
    std::vector<int> cpus = choose_cpus(a, 3);
    g_tsc_hz = calibrate_tsc_hz();
    g_ticks.reserve(g_n - g_warm);

    std::thread t1 = wtc::spawn(generate);
    std::thread t2 = wtc::spawn(transform);
    std::thread t3 = wtc::spawn(sink);
    pin_thread(t1, cpus[0]); pin_thread(t2, cpus[1]); pin_thread(t3, cpus[2]);
    const long f0 = cur_freq_khz(cpus[0]);
    t1.join(); t2.join(); t3.join();
    const long f1 = cur_freq_khz(cpus[0]);

    if (!g_order_ok) { std::fprintf(stderr, "FIFO broken\n"); return 1; }
    const double secs = static_cast<double>(g_t_end - g_t_start) / 1e9;
    const double mrec = static_cast<double>(g_n - g_warm) / secs / 1e6;
    percentiles p = compute_percentiles(g_ticks, 1e9 / g_tsc_hz);

    result_line r;
    r.set("workload", std::string("W3wtc"));
    r.set("mode", g_rate ? std::string("open") : std::string("closed"));
    r.set("variant", flavour);
    r.set("elem", std::string("64"));
    r.set("rate", g_rate);
    r.set("work", g_work);
    r.set("rep", rep);
    r.set("n_ops", g_n - g_warm);
    r.set("n_total", g_n);
    r.set("mops", mrec);
    r.set("p50", p.p50); r.set("p90", p.p90); r.set("p99", p.p99);
    r.set("p999", p.p999); r.set("p9999", p.p9999); r.set("max_ns", p.max);
    r.set("checksum", g_checksum);
    r.set("cpus", std::to_string(cpus[0]) + "," + std::to_string(cpus[1]) + "," + std::to_string(cpus[2]));
    r.set("freq_before_khz", static_cast<std::uint64_t>(f0));
    r.set("freq_after_khz", static_cast<std::uint64_t>(f1));
    r.set("tsc_hz", g_tsc_hz);
    r.set("hist_ns", log_histogram(g_ticks, 1e9 / g_tsc_hz));
    if (g_rate) r.set("saturated", mrec * 1e6 < 0.95 * static_cast<double>(g_rate) ? 1 : 0);
    // Which implementation each queue actually carries, asked of the object
    // rather than read off a disassembly. The tag branch lives inside the
    // shim's member functions, which are the same symbols in every build, so
    // comparing the pipeline's own functions proves nothing about the
    // substitution; this does.
    auto name = [](wtc::topology t) {
        switch (t) {
        case wtc::topology::spsc: return "spsc";
        case wtc::topology::spsc_inline: return "spsc_inline";
        default: return "generic";
        }
    };
    const char *tag1 = name(stage1.tag());
    const char *tag2 = name(stage2.tag());
    r.set("tags", std::string(tag1) + "," + tag2);
    r.write(json);
    std::printf("checksum %llu tags %s,%s\n", static_cast<unsigned long long>(g_checksum), tag1, tag2);
    std::fprintf(stderr, "W3wtc %-9s %7.2f Mrec/s  p50 %6.0f  p99 %7.0f  p99.9 %7.0f ns\n",
                 flavour.c_str(), mrec, p.p50, p.p99, p.p999);
    return 0;
}
