// SPDX-License-Identifier: Unlicense
//
// The g3log leg's program: the loop of g3log's own performance test
// (test_performance/performance.h, doLogWrites: the same LOG(INFO)
// statement, ITERATIONS times) on THREADS threads into one LogWorker, whose
// background thread hands each message to one sink:
//   file  the default FileSink (addDefaultLogger), as the performance
//         test's main_threaded_mean does, writing under DIR
//   null  a sink whose receive() does nothing
//
//   bench THREADS ITERATIONS SINK [DIR]
//
// Prints the producers' time (application), the time until the worker and
// the sink have drained (total), and the drain rate in messages per second.
#include <g3log/g3log.hpp>
#include <g3log/logworker.hpp>
#include <atomic>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <vector>

namespace {
const char* charptrmsg = "\tmessage by char*";
const std::string strmsg{"\tmessage by string"};
float pi_f{3.1415926535897932384626433832795f};
uint64_t g_iterations = 1000000;

struct NullSink {
    void receive(g3::LogMessageMover) {}
};

void doLogWrites(const std::string& title) {
    for (uint64_t count = 0; count < g_iterations; ++count) {
        LOG(INFO) << title << " iteration #" << count << " " << charptrmsg << strmsg << " and a float: " << std::setprecision(6) << pi_f;
    }
}
double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

int main(int argc, char** argv) {
    size_t n = argc > 1 ? atoi(argv[1]) : 1;
    g_iterations = argc > 2 ? strtoull(argv[2], 0, 10) : 1000000;
    std::string sink = argc > 3 ? argv[3] : "null";
    std::string dir = argc > 4 ? argv[4] : "/tmp/";
    auto worker = g3::LogWorker::createLogWorker();
    std::unique_ptr<g3::FileSinkHandle> fh;
    std::unique_ptr<g3::SinkHandle<NullSink>> nh;
    if (sink == "file") fh = worker->addDefaultLogger("g3bench", dir);
    else nh = worker->addSink(std::make_unique<NullSink>(), &NullSink::receive);
    g3::initializeLogging(worker.get());
    struct rusage r0, r1;
    getrusage(RUSAGE_SELF, &r0);
    double t0 = now_s();
    std::vector<std::thread> th;
    for (size_t i = 0; i < n; ++i) th.emplace_back(doLogWrites, "G3LOG_T" + std::to_string(i + 1));
    for (auto& t : th) t.join();
    double t_app = now_s() - t0;
    worker.reset();  // flushes the queues
    double t_tot = now_s() - t0;
    getrusage(RUSAGE_SELF, &r1);
    double cpu = (r1.ru_utime.tv_sec - r0.ru_utime.tv_sec) + (r1.ru_utime.tv_usec - r0.ru_utime.tv_usec) / 1e6 +
                 (r1.ru_stime.tv_sec - r0.ru_stime.tv_sec) + (r1.ru_stime.tv_usec - r0.ru_stime.tv_usec) / 1e6;
    double msgs = (double)n * g_iterations;
    printf("g3 sink=%s threads=%zu msgs=%.0f app_ns/msg=%.0f total_ns/msg=%.0f drain_Mmsg/s=%.3f app_per_thread_ns/msg=%.0f cpu_ns/msg=%.0f vcsw/msg=%.3f maxrss_MB=%ld\n",
           sink.c_str(), n, msgs, t_app / msgs * 1e9, t_tot / msgs * 1e9, msgs / t_tot / 1e6, t_app / g_iterations * 1e9,
           cpu / msgs * 1e9, (double)(r1.ru_nvcsw - r0.ru_nvcsw) / msgs, r1.ru_maxrss / 1024);
    return 0;
}
