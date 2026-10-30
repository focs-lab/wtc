// SPDX-License-Identifier: Apache-2.0
//
// The CTPL leg's program: one submitting thread and a pool of WORKERS
// threads (CTPL's ctpl_stl.h, as released, as lifted, or as ported), tiny
// tasks of WORK_NS of computation each. The main thread submits, keeps its
// last INFLIGHT futures in a ring, and takes each before reusing its
// slot, so at most INFLIGHT tasks are queued or running.
//
//   bench WORKERS [SECONDS] [WORK_NS] [INFLIGHT]
//
// Prints the time per task, tasks per second, CPU per task, the share of
// it in the kernel and the voluntary context switches per task.
#include "ctpl_stl.h"
#define VARIANT "ctpl"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <sys/resource.h>
#include <vector>

static uint64_t g_rounds = 0;
static inline uint64_t work_rounds(uint64_t x, uint64_t rounds) {
    for (uint64_t i = 0; i < rounds; ++i) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        asm volatile("" : "+r"(x));
    }
    return x;
}
static double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static inline uint64_t mix(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

int main(int argc, char** argv) {
    int workers = argc > 1 ? atoi(argv[1]) : 1;
    double secs = argc > 2 ? atof(argv[2]) : 1.0;
    double work_ns = argc > 3 ? atof(argv[3]) : 0;
    size_t inflight = argc > 4 ? atoi(argv[4]) : 4096;
    if (work_ns > 0) {
        auto t0 = now_s();
        volatile uint64_t r = work_rounds(1, 50'000'000);
        (void)r;
        g_rounds = (uint64_t)(work_ns * 50'000'000 / ((now_s() - t0) * 1e9));
    }
    ctpl::thread_pool pool(workers);
    std::vector<std::future<uint64_t>> fut(inflight);
    std::vector<uint64_t> ids(inflight);
    uint64_t bad = 0;
    auto phase = [&](double dur) {
        uint64_t i = 0;
        double t0 = now_s();
        for (;; ++i) {
            if ((i & 255) == 0 && now_s() - t0 >= dur) break;
            size_t k = i % inflight;
            if (i >= inflight) bad += fut[k].get() != mix(ids[k]);
            ids[k] = i;
            fut[k] = pool.push([i](int) { return g_rounds ? work_rounds(i, g_rounds) * 0 + mix(i) : mix(i); });
        }
        for (uint64_t r = i > inflight ? i - inflight : 0; r < i; ++r) bad += fut[r % inflight].get() != mix(ids[r % inflight]);
        return i;
    };
    phase(secs / 10);
    struct rusage r0, r1;
    getrusage(RUSAGE_SELF, &r0);
    double t0 = now_s();
    uint64_t n = phase(secs);
    double t = now_s() - t0;
    getrusage(RUSAGE_SELF, &r1);
    double cpu = (r1.ru_utime.tv_sec - r0.ru_utime.tv_sec) + (r1.ru_utime.tv_usec - r0.ru_utime.tv_usec) / 1e6 +
                 (r1.ru_stime.tv_sec - r0.ru_stime.tv_sec) + (r1.ru_stime.tv_usec - r0.ru_stime.tv_usec) / 1e6;
    double sys = (r1.ru_stime.tv_sec - r0.ru_stime.tv_sec) + (r1.ru_stime.tv_usec - r0.ru_stime.tv_usec) / 1e6;
    printf("%s workers=%d work_ns=%.0f inflight=%zu ns/task=%.1f Mtasks/s=%.3f cpu_ns/task=%.0f sys%%=%.0f vcsw/task=%.3f bad=%lu\n",
           VARIANT, workers, work_ns, inflight, t / n * 1e9, n / t / 1e6, cpu / n * 1e9, cpu > 0 ? 100 * sys / cpu : 0,
           (double)(r1.ru_nvcsw - r0.ru_nvcsw) / n, bad);
    return bad != 0;
}
