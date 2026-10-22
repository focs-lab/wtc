// W10: the ablation. What synchronisation strength buys and what keeping the
// element out of the allocator buys, each on its own and both together.
//
//   w10_ablation --variant spsc_cell --elem 64 --n 20000000
//                [--cpus 4,5 | --smt-siblings] [--json runs.jsonl]
//
// One producer, one consumer, the measurement of W1, over the runtime's two
// queues used directly, each holding the element in its cell and behind a
// shared_ptr:
//
//   mpmc_sp     segmented queue, element on the heap   (the generic tag)
//   mpmc_cell   segmented queue, element in the cell
//   spsc_sp     ring, element on the heap              (the spsc tag)
//   spsc_cell   ring, element in the cell              (the spsc_inline tag)
//
// The two rows that differ in one factor only isolate it: mpmc_sp against
// spsc_sp is the strength of synchronisation with the allocator held
// constant; mpmc_sp against mpmc_cell is the allocator with the
// synchronisation held constant. The compiler's substitutions are the
// diagonal, from mpmc_sp to spsc_cell, and the question the table answers
// is whether either factor alone would have done. The allocator is a
// factor of the leg (campaign.sh preloads it) because the heap rows depend
// on it and the cell rows must not.
//
// The element sizes, the placement on separate cores or on the SMT siblings
// of one core, and the in-flight throttle are W1's.

#include "elements.hpp"
#include "harness.hpp"
#include "segment_queue.hpp"   // runtime, via -I runtime
#include "spsc_ring.hpp"

#include <memory>
#include <thread>

using namespace wtc_bench;

namespace {

template <class T> struct mpmc_sp {
    wtc::rt::segment_queue<std::shared_ptr<T>> q;
    void push(const T& v) { q.push(std::make_shared<T>(v)); }
    bool try_pop(T& out) { std::shared_ptr<T> p; if (!q.try_pop(p)) return false; out = *p; return true; }
};
template <class T> struct mpmc_cell {
    wtc::rt::segment_queue<T> q;
    void push(const T& v) { q.push(v); }
    bool try_pop(T& out) { return q.try_pop(out); }
};
template <class T> struct spsc_sp {
    wtc::rt::spsc_ring<std::shared_ptr<T>> q;
    void push(const T& v) { q.push(std::make_shared<T>(v)); }
    bool try_pop(T& out) { std::shared_ptr<T> p; if (!q.try_pop(p)) return false; out = *p; return true; }
};
template <class T> struct spsc_cell {
    wtc::rt::spsc_ring<T> q;
    void push(const T& v) { q.push(v); }
    bool try_pop(T& out) { return q.try_pop(out); }
};

const char* const kVariants[] = {"mpmc_sp", "mpmc_cell", "spsc_sp", "spsc_cell"};

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
        r.set("workload", std::string("W10"));
        r.set("variant", c.variant);
        r.set("elem", c.elem);
        r.set("wait", c.wait);
        r.set("cpus", std::to_string(c.cpus[0]) + "," + std::to_string(c.cpus[1]));
        r.set("smt", c.smt ? 1 : 0);
        r.set("rep", c.rep);
        r.set("n_ops", n - warm);
        r.set("n_total", n);
        r.set("wall_ns", t_end.load() - t_start.load());
        r.set("mops", mops);
        r.set("ns_per_op", secs * 1e9 / static_cast<double>(n - warm));
        r.set("freq_before_khz", static_cast<std::uint64_t>(f0));
        r.set("freq_after_khz", static_cast<std::uint64_t>(f1));
        r.set("tsc_hz", c.tsc_hz);
        r.write(c.json);
        std::fprintf(stderr, "W10 %-10s E%-4s %s  %8.2f Mops/s\n", c.variant.c_str(), c.elem.c_str(), c.wait.c_str(), mops);
    }
};

template <class E, class F>
bool with_variant(const std::string& name, F&& f) {
    if (name == "mpmc_sp")   { f.template run<mpmc_sp<E>>(); return true; }
    if (name == "mpmc_cell") { f.template run<mpmc_cell<E>>(); return true; }
    if (name == "spsc_sp")   { f.template run<spsc_sp<E>>(); return true; }
    if (name == "spsc_cell") { f.template run<spsc_cell<E>>(); return true; }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    args a(argc, argv);
    config c;
    c.variant = a.get("variant", "spsc_cell");
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
        for (auto v : kVariants) std::fprintf(stderr, " %s", v);
        std::fputc('\n', stderr);
        return 2;
    }
    return 0;
}
