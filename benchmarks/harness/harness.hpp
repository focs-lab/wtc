#pragma once

// Measurement helpers shared by the workloads: CPU selection and pinning,
// the timestamp counter and its calibration, percentiles, the results line.
// Nothing here is clever; what matters is that every workload does these
// things the same way, so a difference between rows is a difference in the
// queue.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <linux/perf_event.h>
#include <map>
#include <sched.h>
#include <pthread.h>
#include <set>
#include <string>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <x86intrin.h>

namespace wtc_bench {

// ---- time ------------------------------------------------------------

inline std::uint64_t rdtsc() { return __rdtsc(); }

// A timestamp for a latency sample. rdtsc alone may execute before the load
// that delivered the element, or after later work has started; the fences
// order it against both, so the sample brackets exactly the operation.
// Pacing loops use the plain read, which is cheaper and needs no order.
inline std::uint64_t rdtsc_fenced() {
    _mm_lfence();
    const std::uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

inline void cpu_relax() { _mm_pause(); }

inline std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// TSC ticks per second, measured against steady_clock over a short sleep.
// The host has constant_tsc and nonstop_tsc; this is a calibration, not a
// guess, and it is recorded in every results line.
inline double calibrate_tsc_hz(int ms = 200) {
    const auto t0 = now_ns(); const auto c0 = rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    const auto t1 = now_ns(); const auto c1 = rdtsc();
    return static_cast<double>(c1 - c0) / (static_cast<double>(t1 - t0) / 1e9);
}

// ---- CPUs ------------------------------------------------------------

inline std::vector<int> allowed_cpus() {
    cpu_set_t set; CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) != 0) { std::perror("sched_getaffinity"); std::exit(2); }
    std::vector<int> out;
    for (int i = 0; i < CPU_SETSIZE; ++i) if (CPU_ISSET(i, &set)) out.push_back(i);
    return out;
}

inline int core_id_of(int cpu) {
    char path[128];
    std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu);
    std::ifstream f(path); int id = -1; f >> id; return id;
}

inline std::vector<int> siblings_of(int cpu) {
    char path[128];
    std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
    std::ifstream f(path); std::string s; std::getline(f, s);
    std::vector<int> out;
    // formats: "4,60" or "4-5"
    for (std::size_t i = 0; i < s.size();) {
        int a = std::atoi(s.c_str() + i);
        while (i < s.size() && s[i] != ',' && s[i] != '-') ++i;
        if (i < s.size() && s[i] == '-') { int b = std::atoi(s.c_str() + i + 1); for (int k = a; k <= b; ++k) out.push_back(k); while (i < s.size() && s[i] != ',') ++i; }
        else out.push_back(a);
        if (i < s.size()) ++i;
    }
    return out;
}

// k CPUs from the allowed set on k distinct physical cores, lowest first.
inline std::vector<int> pick_distinct_cores(int k) {
    std::vector<int> out; std::set<int> cores;
    for (int c : allowed_cpus()) {
        int id = core_id_of(c);
        if (cores.insert(id).second) { out.push_back(c); if ((int)out.size() == k) break; }
    }
    if ((int)out.size() < k) { std::fprintf(stderr, "need %d distinct cores, allowed set has %zu\n", k, out.size()); std::exit(2); }
    return out;
}

// A CPU and its SMT sibling, both in the allowed set.
inline std::vector<int> pick_sibling_pair() {
    for (int c : allowed_cpus()) {
        for (int s : siblings_of(c)) {
            if (s != c) {
                auto a = allowed_cpus();
                if (std::find(a.begin(), a.end(), s) != a.end()) return {c, s};
            }
        }
    }
    std::fprintf(stderr, "no SMT sibling pair in the allowed set\n"); std::exit(2);
}

inline void pin_thread(std::thread& t, int cpu) {
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set);
    int rc = pthread_setaffinity_np(t.native_handle(), sizeof set, &set);
    if (rc != 0) { std::fprintf(stderr, "pthread_setaffinity_np(cpu %d): %s\n", cpu, std::strerror(rc)); std::exit(2); }
}

inline void pin_self(int cpu) {
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof set, &set) != 0) { std::perror("sched_setaffinity"); std::exit(2); }
}

inline long cur_freq_khz(int cpu) {
    char path[128];
    std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
    std::ifstream f(path); long v = -1; f >> v; return v;
}

// ---- statistics ------------------------------------------------------

struct percentiles {
    double p50, p90, p99, p999, p9999, max;
};

inline percentiles compute_percentiles(std::vector<std::uint64_t>& v, double scale) {
    if (v.empty()) return {0, 0, 0, 0, 0, 0};
    std::sort(v.begin(), v.end());
    auto at = [&](double q) { std::size_t i = static_cast<std::size_t>(q * (v.size() - 1)); return v[i] * scale; };
    return {at(0.50), at(0.90), at(0.99), at(0.999), at(0.9999), v.back() * scale};
}

// The whole distribution, not only its percentiles: the samples counted in
// log-linear buckets of the scaled value (ns), eight per power of two, so a
// bucket is at most 12.5% wide. Written as "lower:count" pairs for the
// non-empty buckets, which is enough to draw the distribution and to read
// any percentile back to within a bucket. Expects v sorted, as
// compute_percentiles leaves it.
inline std::string log_histogram(const std::vector<std::uint64_t>& v, double scale) {
    constexpr int kSub = 8;
    auto lower_of = [](double x) {
        if (x < 1.0) return 0.0;
        int e = 0;
        double m = std::frexp(x, &e);           // x = m * 2^e, m in [0.5, 1)
        double base = std::ldexp(1.0, e - 1);   // the power of two below x
        double frac = (m * 2.0) - 1.0;          // position within [base, 2*base)
        return base * (1.0 + std::floor(frac * kSub) / kSub);
    };
    std::string out;
    double cur = -1.0;
    std::uint64_t count = 0;
    auto flush = [&] {
        if (count == 0) return;
        char b[64];
        std::snprintf(b, sizeof b, "%s%.6g:%llu", out.empty() ? "" : ",", cur,
                      static_cast<unsigned long long>(count));
        out += b;
    };
    for (std::uint64_t t : v) {
        const double lo = lower_of(static_cast<double>(t) * scale);
        if (lo != cur) { flush(); cur = lo; count = 0; }
        ++count;
    }
    flush();
    return out;
}

// ---- in-flight throttle ---------------------------------------------

// Unbounded queues would otherwise measure the allocator: the producer runs
// ahead until memory is the bottleneck. The consumer publishes its count
// every kStride pops; the producer checks every kStride pushes and waits
// while more than kMaxInFlight elements are queued. Bounded queues see the
// same limit, so every row measures the same shape.
struct throttle {
    static constexpr std::uint64_t kStride = 1024;
    static constexpr std::uint64_t kMaxInFlight = 4096;
    alignas(64) std::atomic<std::uint64_t> consumed{0};
    alignas(64) char pad[64];
    void producer_wait(std::uint64_t pushed) {
        if (pushed % kStride != 0) return;
        while (pushed - consumed.load(std::memory_order_relaxed) > kMaxInFlight) cpu_relax();
    }
    void consumer_progress(std::uint64_t popped) {
        if (popped % kStride == 0) consumed.store(popped, std::memory_order_relaxed);
    }
};

// ---- results ---------------------------------------------------------

// The process's peak resident set so far, in KiB. Measured at the end of a
// run it bounds what the queue held at its fullest, which is the memory
// cost an unbounded queue trades for never blocking.
inline std::uint64_t peak_rss_kib() {
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
    return static_cast<std::uint64_t>(ru.ru_maxrss);
}

// ---- runs that explain a result ----------------------------------------

// A timed run says how fast; these say why, and are kept apart from it
// because measuring perturbs what is measured. WTC_PASS names the pass in
// every line ("mechanism"), so the summary never mixes the two.
//
// Hardware counters (WTC_PERF=1): opened in the main thread before any
// worker starts, user space only (what perf_event_paranoid 2 allows), and
// inherited, so each counts every thread of the process; a thread's share
// is added when it exits, which every workload's threads have done by the
// time the line is written. A counter the machine refuses is left out.
struct perf_session {
    struct counter { const char* name; std::uint32_t type; std::uint64_t config; int fd; };
    std::vector<counter> counters;
    perf_session() {
        if (!std::getenv("WTC_PERF")) return;
        const std::uint64_t llc_read_miss =
            PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);
        counters = {
            {"cycles", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, -1},
            {"instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, -1},
            {"cache_misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES, -1},
            {"llc_read_misses", PERF_TYPE_HW_CACHE, llc_read_miss, -1},
        };
        for (auto& c : counters) {
            perf_event_attr attr;
            std::memset(&attr, 0, sizeof attr);
            attr.size = sizeof attr;
            attr.type = c.type;
            attr.config = c.config;
            attr.inherit = 1;
            attr.exclude_kernel = 1;
            attr.exclude_hv = 1;
            c.fd = static_cast<int>(syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0));
        }
    }
    void add_to(std::map<std::string, std::string>& out) const {
        for (auto& c : counters) {
            std::uint64_t v = 0;
            if (c.fd >= 0 && read(c.fd, &v, sizeof v) == static_cast<ssize_t>(sizeof v))
                out[c.name] = std::to_string(v);
        }
    }
};

inline perf_session g_perf;

// Allocator counts, when the run has the counting preload
// (harness/alloccount.cpp); found at run time, so nothing links to it.
inline void add_alloc_counts(std::map<std::string, std::string>& out) {
    using counts_fn = void (*)(unsigned long*, unsigned long*, unsigned long*);
    auto f = reinterpret_cast<counts_fn>(dlsym(RTLD_DEFAULT, "wtc_alloc_counts"));
    if (!f) return;
    unsigned long allocs = 0, frees = 0, bytes = 0;
    f(&allocs, &frees, &bytes);
    out["allocs"] = std::to_string(allocs);
    out["frees"] = std::to_string(frees);
    out["alloc_bytes"] = std::to_string(bytes);
}

// One JSON object per line, appended. Keys are fixed so summarise.py needs
// no schema; values are numbers or strings. Every line carries the
// process's peak resident set at the time it is written, and, in a run that
// explains rather than times, its pass and whatever it counted.
struct result_line {
    std::map<std::string, std::string> fields;
    void set(const std::string& k, const std::string& v) { fields[k] = "\"" + v + "\""; }
    void set(const std::string& k, double v) { char b[64]; std::snprintf(b, sizeof b, "%.6g", v); fields[k] = b; }
    void set(const std::string& k, std::uint64_t v) { fields[k] = std::to_string(v); }
    void set(const std::string& k, int v) { fields[k] = std::to_string(v); }
    void write(const std::string& path) const {
        std::string line = "{";
        bool first = true;
        auto all = fields;
        if (!all.count("peak_rss_kib")) all["peak_rss_kib"] = std::to_string(peak_rss_kib());
        if (const char* pass = std::getenv("WTC_PASS")) all["pass"] = std::string("\"") + pass + "\"";
        g_perf.add_to(all);
        add_alloc_counts(all);
        for (auto& [k, v] : all) { if (!first) line += ", "; first = false; line += "\"" + k + "\": " + v; }
        line += "}\n";
        if (path.empty()) { std::fputs(line.c_str(), stdout); return; }
        std::ofstream f(path, std::ios::app); f << line;
    }
};

// ---- arguments -------------------------------------------------------

struct args {
    std::map<std::string, std::string> kv;
    args(int argc, char** argv) {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a.rfind("--", 0) != 0) continue;
            auto eq = a.find('=');
            if (eq != std::string::npos) kv[a.substr(2, eq - 2)] = a.substr(eq + 1);
            else if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) kv[a.substr(2)] = argv[++i];
            else kv[a.substr(2)] = "1";
        }
    }
    std::string get(const std::string& k, const std::string& d) const { auto it = kv.find(k); return it == kv.end() ? d : it->second; }
    std::uint64_t getu(const std::string& k, std::uint64_t d) const { auto it = kv.find(k); return it == kv.end() ? d : std::strtoull(it->second.c_str(), nullptr, 10); }
    bool has(const std::string& k) const { return kv.count(k) != 0; }
};

// "spin" busy-waits with pause; "yield" gives the CPU up on every miss.
inline void wait_policy(const std::string& policy, unsigned& misses) {
    ++misses;
    if (policy == "yield") std::this_thread::yield();
    else cpu_relax();
}

// Parses "4,5" into cpus; falls back to k distinct cores, or a sibling pair.
inline std::vector<int> choose_cpus(const args& a, int k) {
    std::string s = a.get("cpus", "");
    if (!s.empty()) {
        std::vector<int> out; std::size_t i = 0;
        while (i < s.size()) { out.push_back(std::atoi(s.c_str() + i)); while (i < s.size() && s[i] != ',') ++i; if (i < s.size()) ++i; }
        if ((int)out.size() != k) { std::fprintf(stderr, "--cpus needs %d entries\n", k); std::exit(2); }
        return out;
    }
    if (a.has("smt-siblings") && k == 2) return pick_sibling_pair();
    return pick_distinct_cores(k);
}

}
