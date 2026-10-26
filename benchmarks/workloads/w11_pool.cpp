// W11: a thread pool's tasks, on the heap or in the queue's cell.
//
//   w11_pool --variant cell_spmc --elem 64 --consumers 4 --work 100
//            [--rounds R] [--seconds 1 | --n N] [--cpus 16,17,18,19,20]
//            [--rep 0] [--json runs.jsonl]
//   w11_pool --calibrate        prints rounds of the work per ns
//
// One submitter and --consumers workers. A task is a callable with a
// capture of --elem bytes (16, 64 or 256) that does --work ns of dependent
// arithmetic (--rounds of it exactly, when given). Four variants, two
// factors:
//
//   heap_mpmc   std::function<void(acc&)> through the segmented queue for
//               many consumers: how a pool is written. A capture larger
//               than std::function's small buffer (16 bytes in libstdc++,
//               for a trivially copyable capture) is allocated by the
//               submitter and freed by whichever worker ran it; at 16
//               bytes the capture stays inline, and the row isolates the
//               indirect call from the allocation.
//   heap_spmc   the same tasks through the ring for one producer and many
//               consumers, which is what a pool's topology allows.
//   cell_mpmc   the task by value in the queue's cell, a function pointer
//               and the capture, moved out by the worker: no allocation per
//               task, only the queue's own storage (a segment of 1024 cells
//               for the segmented queue, a block of the ring), amortised
//               over the tasks it holds; what a compiler that proved the
//               worker only runs and drops the task could choose.
//   cell_spmc   both.
//
// Every word of the capture is folded into the task's arithmetic, a few
// operations on top of the --work rounds, so each variant reads the whole
// capture: the heap rows from the allocation, the cell rows from the cell. Each worker adds up the ids of the tasks it ran,
// the count, and a sum and an XOR of a mixing hash of each id; the totals
// must equal those of every id submitted once, or the run fails. The
// submitter pushes until --seconds have passed after a warm-up of a tenth
// of that (or --n tasks, the first tenth of them warm-up), then one stop
// task per worker. The time runs from the end of the warm-up to the last
// worker's exit, and the tasks it counts are those the workers had not yet
// finished when it started (by their published counts), so the backlog
// the warm-up left is counted with the time spent draining it. Tasks in
// flight are bounded: every 1024 pushes the submitter waits while the
// workers' counts, which they publish every 256 tasks, leave more than
// 8192 + 256 per worker queued, so at most 9215 + 256 per worker are, and
// the counts' lag can never hold the submitter back by itself.

#include "harness.hpp"
#include "segment_queue.hpp"   // runtime, via -I runtime
#include "spmc_ring.hpp"

#include <array>
#include <functional>
#include <thread>

using namespace wtc_bench;

namespace {

std::uint64_t g_rounds = 0;   // per task

constexpr std::uint64_t kStop = ~std::uint64_t{0};

inline std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

template <std::size_t Cap> struct capture {
    static_assert(Cap % 8 == 0 && Cap >= 16);
    std::array<std::uint64_t, Cap / 8> w;
    std::uint64_t id() const { return w[0]; }
    static capture of(std::uint64_t id) {
        capture c;
        c.w[0] = id;
        for (std::size_t i = 1; i < c.w.size(); ++i) c.w[i] = mix(id + i);
        return c;
    }
};

// What a worker's tasks add up to; the arithmetic's result goes to sink so
// it is not optimised away.
struct acc {
    std::uint64_t count = 0, ids = 0, hash_sum = 0, hash_xor = 0, sink = 0;
    bool stop = false;
};

template <std::size_t Cap> void body(const capture<Cap>& c, acc& a) {
    const std::uint64_t id = c.id();
    if (id == kStop) { a.stop = true; return; }
    std::uint64_t x = 0;
    for (std::uint64_t v : c.w) x = (x ^ v) * 0x9e3779b97f4a7c15ULL;
    a.sink += work_rounds(x, g_rounds);
    const std::uint64_t h = mix(id);
    ++a.count; a.ids += id; a.hash_sum += h; a.hash_xor ^= h;
}

template <std::size_t Cap> struct heap_task {
    std::function<void(acc&)> f;
    static heap_task make(std::uint64_t id) {
        return {[c = capture<Cap>::of(id)](acc& a) { body(c, a); }};
    }
    void run(acc& a) { f(a); f = nullptr; }
};

template <std::size_t Cap> struct cell_task {
    void (*fn)(const capture<Cap>&, acc&);
    capture<Cap> c;
    static cell_task make(std::uint64_t id) { return {&body<Cap>, capture<Cap>::of(id)}; }
    void run(acc& a) { fn(c, a); }
};

template <class T> using via_mpmc = wtc::rt::segment_queue<T, true>;
template <class T> using via_spmc = wtc::rt::spmc_ring<T>;

const char* const kVariants[] = {"heap_mpmc", "heap_spmc", "cell_mpmc", "cell_spmc"};

struct config {
    std::string variant, json;
    std::size_t elem;
    int consumers, rep;
    std::uint64_t n, work_ns;      // n == 0: run for `seconds`
    double seconds, tsc_hz;
    std::vector<int> cpus;
};

// Per-worker progress on its own lines, read by the submitter's throttle.
struct alignas(128) progress { std::atomic<std::uint64_t> done{0}; };

template <class Task, class Q> int run(const config& c) {
    Q q;
    std::vector<progress> prog(c.consumers);
    std::vector<acc> res(c.consumers);
    std::atomic<int> ready{0};
    std::vector<std::thread> ws;
    for (int w = 0; w < c.consumers; ++w) {
        ws.emplace_back([&, w] {
            Task t;
            acc a;
            ready.fetch_add(1);
            for (;;) {
                if (!q.try_pop(t)) { cpu_relax(); continue; }
                t.run(a);
                if (a.stop) break;
                if ((a.count & 255) == 0) prog[w].done.store(a.count, std::memory_order_relaxed);
            }
            prog[w].done.store(a.count, std::memory_order_relaxed);
            res[w] = a;
        });
        pin_thread(ws.back(), c.cpus[1 + w]);
    }
    pin_self(c.cpus[0]);
    while (ready.load() < c.consumers) cpu_relax();

    const long f0 = cur_freq_khz(c.cpus[0]);
    const std::uint64_t t_go = now_ns();
    const std::uint64_t warm_ns = static_cast<std::uint64_t>(c.seconds * 1e8);   // a tenth
    const std::uint64_t run_ns = static_cast<std::uint64_t>(c.seconds * 1e9);
    const std::uint64_t warm_n = c.n / 10;
    const std::uint64_t bound = 8192 + 256 * static_cast<std::uint64_t>(c.consumers);
    auto done_now = [&] {
        std::uint64_t done = 0;
        for (auto& p : prog) done += p.done.load(std::memory_order_relaxed);
        return done;
    };
    std::uint64_t t_start = 0, done_start = 0, i = 0;
    bool warming = true;
    for (;; ++i) {
        if (c.n) {
            if (warming && i == warm_n) { t_start = now_ns(); done_start = done_now(); warming = false; }
            if (i == c.n) break;
        }
        q.push(Task::make(i));
        if ((i & 1023) == 1023) {
            if (!c.n) {
                const std::uint64_t now = now_ns();
                if (warming && now - t_go >= warm_ns) { t_start = now; done_start = done_now(); warming = false; }
                else if (!warming && now - t_start >= run_ns) { ++i; break; }
            }
            while (i + 1 - done_now() > bound) cpu_relax();
        }
    }
    const std::uint64_t n_total = i;
    for (int w = 0; w < c.consumers; ++w) q.push(Task::make(kStop));
    for (auto& t : ws) t.join();
    const std::uint64_t t_end = now_ns();
    const long f1 = cur_freq_khz(c.cpus[0]);

    acc sum;
    for (auto& a : res) {
        sum.count += a.count; sum.ids += a.ids; sum.hash_sum += a.hash_sum; sum.hash_xor ^= a.hash_xor; sum.sink += a.sink;
    }
    acc want;
    for (std::uint64_t k = 0; k < n_total; ++k) {
        const std::uint64_t h = mix(k);
        ++want.count; want.ids += k; want.hash_sum += h; want.hash_xor ^= h;
    }
    if (sum.count != want.count || sum.ids != want.ids || sum.hash_sum != want.hash_sum || sum.hash_xor != want.hash_xor) {
        std::fprintf(stderr, "W11 %s: the workers ran %llu tasks, %llu were submitted, or their ids differ\n",
                     c.variant.c_str(), (unsigned long long)sum.count, (unsigned long long)n_total);
        return 1;
    }
    const std::uint64_t n_ops = n_total - done_start;
    const double secs = static_cast<double>(t_end - t_start) / 1e9;
    const double mops = static_cast<double>(n_ops) / secs / 1e6;

    std::string cpus;
    for (int x : c.cpus) cpus += (cpus.empty() ? "" : ",") + std::to_string(x);
    result_line r;
    r.set("workload", std::string("W11"));
    r.set("variant", c.variant);
    r.set("elem", std::to_string(c.elem));
    r.set("producers", 1);
    r.set("consumers", c.consumers);
    r.set("work", c.work_ns);
    r.set("rounds", g_rounds);
    r.set("cpus", cpus);
    r.set("rep", c.rep);
    r.set("n_ops", n_ops);
    r.set("n_total", n_total);
    r.set("wall_ns", t_end - t_start);
    r.set("mops", mops);
    r.set("ns_per_op", secs * 1e9 / static_cast<double>(n_ops));
    r.set("freq_before_khz", static_cast<std::uint64_t>(f0));
    r.set("freq_after_khz", static_cast<std::uint64_t>(f1));
    r.set("tsc_hz", c.tsc_hz);
    r.set("sink", sum.sink & 0xffff);
    r.write(c.json);
    std::fprintf(stderr, "W11 %-9s E%-4zu consumers %d work %4llu ns  %8.3f Mtasks/s\n", c.variant.c_str(), c.elem,
                 c.consumers, (unsigned long long)c.work_ns, mops);
    return 0;
}

template <std::size_t Cap> int with_variant(const config& c) {
    if (c.variant == "heap_mpmc") return run<heap_task<Cap>, via_mpmc<heap_task<Cap>>>(c);
    if (c.variant == "heap_spmc") return run<heap_task<Cap>, via_spmc<heap_task<Cap>>>(c);
    if (c.variant == "cell_mpmc") return run<cell_task<Cap>, via_mpmc<cell_task<Cap>>>(c);
    if (c.variant == "cell_spmc") return run<cell_task<Cap>, via_spmc<cell_task<Cap>>>(c);
    std::fprintf(stderr, "unknown variant %s; known:", c.variant.c_str());
    for (auto v : kVariants) std::fprintf(stderr, " %s", v);
    std::fputc('\n', stderr);
    return 2;
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
    c.variant = a.get("variant", "cell_spmc");
    c.json = a.get("json", "");
    c.elem = a.getu("elem", 64);
    c.consumers = static_cast<int>(a.getu("consumers", 1));
    c.work_ns = a.getu("work", 0);
    c.rep = static_cast<int>(a.getu("rep", 0));
    c.n = a.getu("n", 0);
    c.seconds = std::atof(a.get("seconds", "1").c_str());
    if (c.consumers < 1) { std::fprintf(stderr, "--consumers must be at least 1\n"); return 2; }
    if (!c.n && c.seconds <= 0) { std::fprintf(stderr, "--seconds must be positive\n"); return 2; }
    c.cpus = choose_cpus(a, c.consumers + 1);
    c.tsc_hz = calibrate_tsc_hz();
    // --rounds fixes the work exactly, the same in every process (a leg
    // calibrates once and passes it); otherwise --work is converted here.
    g_rounds = a.has("rounds") ? a.getu("rounds", 0)
                               : static_cast<std::uint64_t>(static_cast<double>(c.work_ns) * work_rounds_per_ns() + 0.5);

    if (c.elem == 16) return with_variant<16>(c);
    if (c.elem == 64) return with_variant<64>(c);
    if (c.elem == 256) return with_variant<256>(c);
    std::fprintf(stderr, "--elem must be 16, 64 or 256\n");
    return 2;
}
