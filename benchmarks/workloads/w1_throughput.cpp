// W1: one producer, one consumer, n elements, throughput.
//
//   w1_throughput --variant shim_spsc --elem 64 --n 20000000 --wait spin
//                 [--cpus 4,5 | --smt-siblings] [--json runs.jsonl]
//
// The producer pushes n elements carrying their sequence number; the
// consumer pops them and checks the sequence. Timing starts when the
// producer pushes the first element after warm-up and ends when the consumer
// receives the last, so the figure is elements per second through the queue
// with both sides running. The in-flight throttle keeps unbounded queues
// from measuring the allocator instead.

#include "adapters.hpp"
#include "elements.hpp"
#include "harness.hpp"

#include <thread>

using namespace wtc_bench;

namespace {

struct config {
    std::string variant, elem, wait, json;
    std::uint64_t n, warm;
    std::vector<int> cpus;
    double tsc_hz;
    int rep;
    bool smt;
};

template <class E>
struct runner {
    const config& c;
    explicit runner(const config& cfg) : c(cfg) {}

    template <class Q> void run() {
        Q q;
        throttle th;
        std::atomic<std::uint64_t> t_start{0}, t_end{0};
        std::atomic<bool> order_ok{true};
        const std::uint64_t n = c.n, warm = c.warm;

        std::thread prod([&] {
            E e{};
            for (std::uint64_t i = 0; i < n; ++i) {
                if (i == warm) t_start.store(now_ns(), std::memory_order_relaxed);
                e.seq = i;
                q.push(e);
                th.producer_wait(i + 1);
            }
        });
        std::thread cons([&] {
            E e{};
            unsigned misses = 0;
            std::uint64_t got = 0;
            while (got < n) {
                if (!q.try_pop(e)) { wait_policy(c.wait, misses); continue; }
                if (e.seq != got) { order_ok.store(false); break; }
                ++got;
                th.consumer_progress(got);
            }
            t_end.store(now_ns(), std::memory_order_relaxed);
        });
        pin_thread(prod, c.cpus[0]);
        pin_thread(cons, c.cpus[1]);
        const long f0 = cur_freq_khz(c.cpus[0]);
        prod.join(); cons.join();
        const long f1 = cur_freq_khz(c.cpus[0]);

        if (!order_ok.load()) { std::fprintf(stderr, "FIFO broken for %s\n", c.variant.c_str()); std::exit(1); }
        const double secs = static_cast<double>(t_end.load() - t_start.load()) / 1e9;
        const double mops = static_cast<double>(n - warm) / secs / 1e6;

        result_line r;
        r.set("workload", std::string("W1"));
        r.set("variant", c.variant);
        r.set("elem", c.elem);
        r.set("wait", c.wait);
        r.set("cpus", std::to_string(c.cpus[0]) + "," + std::to_string(c.cpus[1]));
        r.set("smt", c.smt ? 1 : 0);
        r.set("rep", c.rep);
        r.set("n_ops", n - warm);
        r.set("wall_ns", t_end.load() - t_start.load());
        r.set("mops", mops);
        r.set("freq_before_khz", static_cast<std::uint64_t>(f0));
        r.set("freq_after_khz", static_cast<std::uint64_t>(f1));
        r.set("tsc_hz", c.tsc_hz);
        r.write(c.json);
        std::fprintf(stderr, "W1 %-22s E%-4s %s  %8.2f Mops/s\n", c.variant.c_str(), c.elem.c_str(), c.wait.c_str(), mops);
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
    c.n = a.getu("n", 20'000'000);
    c.warm = a.getu("warm", c.n / 10);
    c.rep = static_cast<int>(a.getu("rep", 0));
    c.smt = a.has("smt-siblings");
    c.cpus = choose_cpus(a, 2);
    c.tsc_hz = calibrate_tsc_hz();

    bool ok = false;
    if (c.elem == "8") ok = with_variant<E8>(c.variant, runner<E8>(c));
    else if (c.elem == "64") ok = with_variant<E64>(c.variant, runner<E64>(c));
    else if (c.elem == "256") ok = with_variant<E256>(c.variant, runner<E256>(c));
    else { std::fprintf(stderr, "--elem must be 8, 64 or 256\n"); return 2; }
    if (!ok) {
        std::fprintf(stderr, "unknown variant %s; known:", c.variant.c_str());
        for (auto& v : variant_names()) std::fprintf(stderr, " %s", v.c_str());
        std::fputc('\n', stderr);
        return 2;
    }
    return 0;
}
