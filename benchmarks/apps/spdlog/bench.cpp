// SPDX-License-Identifier: MIT
// The spdlog leg's program: an asynchronous logger fed by several threads.
//
//   spdlog-bench --threads N --n M [--payload 8|64|256] [--workers 1]
//                [--sink null|file] [--file PATH] [--queue 8192]
//                [--cpus a,b,...] [--json F] [--rep R] [--variant NAME]
//
// Every logging thread writes M messages of the given payload through one
// asynchronous logger whose pool has the given number of workers (one by
// default: the common configuration, and the one the compiler can turn
// into a single-consumer queue when it is set at run time, as here, from
// the command line). The line records the messages per second from the
// first log call to the moment the last message has been written by the
// workers (the flush after the loops returns when they have), and the
// latency of the log call itself on the logging threads, which is the
// cost a program pays for logging: p50, p99 and p99.9 over every call,
// warm-up excluded. The pool's threads and the logging threads are pinned
// to the given processors in that order, or to distinct cores of the
// reservation, as every workload does.
//
// Message layout follows spdlog's own async benchmark: one format
// argument, a string of the payload's length; the pattern is the message
// alone, so that the time goes to the path and not to formatting a
// timestamp on the worker.

#include "spdlog/async.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/null_sink.h"
#include "spdlog/spdlog.h"

#include "harness.hpp"

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace wtc_bench;

int main(int argc, char** argv) {
    args a(argc, argv);
    const int threads = static_cast<int>(a.getu("threads", 1));
    const std::uint64_t n = a.getu("n", 1'000'000);
    const unsigned payload = static_cast<unsigned>(a.getu("payload", 64));
    const unsigned workers = static_cast<unsigned>(a.getu("workers", 1));
    const std::size_t queue = a.getu("queue", 8192);
    const std::string sink = a.get("sink", "null");
    const std::string file = a.get("file", "/tmp/spdlog-bench.log");
    const std::string json = a.get("json", "");
    const std::string variant = a.get("variant", "spdlog");
    const int rep = static_cast<int>(a.getu("rep", 0));
    const double tsc_hz = calibrate_tsc_hz();
    // The workers first, then the logging threads; from --cpus, or from
    // the reservation's distinct cores.
    const std::vector<int> cpus = choose_cpus(a, static_cast<int>(workers) + threads);

    // The pool's workers are pinned from inside their start hook: that is
    // the one place the port has a hold on them.
    std::atomic<unsigned> worker_index{0};
    spdlog::init_thread_pool(queue, workers, [&] {
        const unsigned i = worker_index.fetch_add(1);
        if (i < cpus.size()) pin_self(cpus[i]);
    });
    std::shared_ptr<spdlog::logger> log;
    if (sink == "file") {
        log = spdlog::basic_logger_mt<spdlog::async_factory>("bench", file, true);
    } else {
        log = spdlog::create_async<spdlog::sinks::null_sink_mt>("bench");
    }

    // The message as written: the leg measures the path from the call to
    // the sink, not the formatting of a timestamp and a level on the way.
    log->set_pattern("%v");

    const std::string msg(payload, 'x');
    const std::uint64_t warm = n / 10;
    std::vector<std::vector<std::uint64_t>> samples(threads);
    for (auto& s : samples) s.reserve(n - warm);
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<std::uint64_t> t_first{~0ull};

    std::vector<std::thread> loggers;
    for (int t = 0; t < threads; ++t) {
        loggers.emplace_back([&, t] {
            if (workers + t < cpus.size()) pin_self(cpus[workers + t]);
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) cpu_relax();
            std::uint64_t first = 0;
            for (std::uint64_t i = 0; i < n; ++i) {
                const std::uint64_t c0 = rdtsc_fenced();
                if (i == warm) first = now_ns();
                log->info("{}", msg);
                const std::uint64_t c1 = rdtsc_fenced();
                if (i >= warm) samples[t].push_back(c1 - c0);
            }
            // The earliest first-measured log call across the threads.
            std::uint64_t seen = t_first.load();
            while (first < seen && !t_first.compare_exchange_weak(seen, first)) {}
        });
    }
    while (ready.load() < threads) cpu_relax();
    go.store(true, std::memory_order_release);
    for (auto& th : loggers) th.join();
    log->flush();  // returns once the workers have written everything
    const std::uint64_t t_end = now_ns();
    const double secs = static_cast<double>(t_end - t_first.load()) / 1e9;
    const double total = static_cast<double>(n - warm) * threads;

    std::vector<std::uint64_t> all;
    for (auto& s : samples) all.insert(all.end(), s.begin(), s.end());
    const percentiles p = compute_percentiles(all, 1e9 / tsc_hz);

    result_line r;
    r.set("workload", std::string("spdlog"));
    r.set("variant", variant);
    r.set("threads", threads);
    r.set("workers", static_cast<std::uint64_t>(workers));
    r.set("payload", static_cast<std::uint64_t>(payload));
    r.set("sink", sink);
    r.set("n", n);
    r.set("rep", rep);
    r.set("mops", total / secs / 1e6);
    r.set("p50", p.p50);
    r.set("p99", p.p99);
    r.set("p999", p.p999);
    r.set("max", p.max);
    r.set("tsc_hz", tsc_hz);
    r.write(json);
    std::fprintf(stderr, "spdlog %-14s %2d threads %u workers payload %-4u %8.2f Mmsg/s  call p50 %.0f p99 %.0f p99.9 %.0f ns\n",
                 variant.c_str(), threads, workers, payload, total / secs / 1e6, p.p50, p.p99, p.p999);
    spdlog::shutdown();
    return 0;
}
