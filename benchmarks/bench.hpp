#pragma once

// Shared by every workload in benchmarks/workloads/.
//
// Each workload is one source compiled two ways. Without WTC_BENCH_DIRECT
// it uses wtc::queue<T>, and run.sh builds it as written (baseline) and
// through --select-queue-impl (optimized). With WTC_BENCH_DIRECT it uses
// wtc::runtime::spsc_queue<T> directly and pops without allocating: the
// hand-written ceiling.
//
// The queue is a global, touched only by push and by BENCH_POP at the call
// sites. That is not style: --select-queue-impl substitutes a global only
// when it can retarget every use, and passing the queue to a helper is a
// use it cannot. A workload written with helpers would silently measure
// nothing.
//
// Every workload checks what it received and exits non-zero if anything
// was lost, duplicated or reordered: a fast wrong queue is not a result.
// It then prints one RESULT line, which run.sh collects.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#ifdef WTC_BENCH_DIRECT
#include "../runtime/spsc_queue.hpp"
template <typename T>
using bench_queue = wtc::runtime::spsc_queue<T>;
#define BENCH_POP(q) (q).try_pop_optional()
#else
#include "queue.hpp"
template <typename T>
using bench_queue = wtc::queue<T>;
#define BENCH_POP(q) (q).try_pop()
#endif

namespace bench {

inline std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

[[noreturn]] inline void fail(const char *workload, const char *what, long long detail) {
    std::fprintf(stderr, "FAIL %s: %s (%lld)\n", workload, what, detail);
    std::exit(1);
}

// The value at quantile q of sorted, by the nearest-rank method.
inline std::int64_t quantile(const std::vector<std::int64_t> &sorted, double q) {
    if (sorted.empty())
        return 0;
    std::size_t rank = static_cast<std::size_t>(q * static_cast<double>(sorted.size()));
    if (rank >= sorted.size())
        rank = sorted.size() - 1;
    return sorted[rank];
}

// One line, key=value pairs, for run.sh to collect. Latency fields appear
// only for workloads that measure latency.
inline void report(const char *workload, long items, std::int64_t elapsed_ns,
                   std::vector<std::int64_t> *latencies = nullptr) {
    double seconds = static_cast<double>(elapsed_ns) / 1e9;
    std::printf("RESULT workload=%s items=%ld elapsed_ns=%lld items_per_sec=%.0f",
                workload, items, static_cast<long long>(elapsed_ns),
                static_cast<double>(items) / seconds);
    if (latencies) {
        std::sort(latencies->begin(), latencies->end());
        std::printf(" p50_ns=%lld p99_ns=%lld p999_ns=%lld max_ns=%lld",
                    static_cast<long long>(quantile(*latencies, 0.50)),
                    static_cast<long long>(quantile(*latencies, 0.99)),
                    static_cast<long long>(quantile(*latencies, 0.999)),
                    static_cast<long long>(latencies->back()));
    }
    std::printf("\n");
}

}
