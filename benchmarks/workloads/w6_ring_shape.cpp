// W6: how the ring's own shape affects it. Block size and cache-line
// padding are template parameters, so they cannot be command-line options;
// this workload instantiates a fixed set and reports each as its own row.
//
//   w6_ring_shape --config K [--n N] [--cpus a,b] [--json F]
//   w6_ring_shape --list
//
// One configuration per process, selected by --config. The first version ran
// all nine in one process and the numbers were unusable: every row spanned a
// factor of seven and the medians said nothing, because nine measurements
// sharing one heap, one set of pages and one warmed cache are not nine
// independent samples of anything.
//
// The block size matters more than it looks. Below a few thousand slots the
// ring had two stable regimes on this host, one of them tens of times
// slower, and which one a run fell into depended on timing rather than on
// anything in the program: once the consumer settled in right behind the
// producer, each line the producer wrote was still being read by the
// consumer and crossed the cores once per element. The ring's consumer now
// keeps its distance (the slip in spsc_ring.hpp), and the default block is
// set in bytes, 128 KiB, so 2048 of these 64-byte elements; this workload
// is what would notice if either choice stopped being right.
//
// The shared_ptr rows exist to show the other half: once every element
// costs an allocation, the ring's shape stops mattering at all, because the
// allocator dominates whatever the ring does.

#include "spsc_ring.hpp"

#include "elements.hpp"
#include "harness.hpp"

#include <memory>
#include <thread>

using namespace wtc_bench;

namespace {

struct config {
    std::string json;
    std::uint64_t n;
    std::vector<int> cpus;
    double tsc_hz;
    int rep;
};

template <class Slot, unsigned kSlots, unsigned kLine, class Make, class Read>
double run_one(const config& c, Make make, Read read) {
    wtc::rt::spsc_ring<Slot, kSlots, kLine> q;
    throttle th;
    std::atomic<std::uint64_t> t0{0}, t1{0};
    std::atomic<bool> ok{true};
    const std::uint64_t n = c.n, warm = n / 10;

    std::thread prod([&] {
        for (std::uint64_t i = 0; i < n; ++i) {
            if (i == warm) t0.store(now_ns(), std::memory_order_relaxed);
            q.push(make(i));
            th.producer_wait(i + 1);
        }
    });
    std::thread cons([&] {
        Slot s{};
        std::uint64_t got = 0;
        while (got < n) {
            if (!q.try_pop(s)) { cpu_relax(); continue; }
            if (read(s) != got) { ok.store(false); break; }
            ++got;
            th.consumer_progress(got);
        }
        t1.store(now_ns(), std::memory_order_relaxed);
    });
    pin_thread(prod, c.cpus[0]);
    pin_thread(cons, c.cpus[1]);
    prod.join(); cons.join();
    if (!ok.load()) { std::fprintf(stderr, "FIFO broken at kSlots=%u kLine=%u\n", kSlots, kLine); std::exit(1); }
    return static_cast<double>(n - warm) / (static_cast<double>(t1.load() - t0.load()) / 1e9) / 1e6;
}

void report(const config& c, const char* slot, unsigned slots, unsigned line, double mops) {
    result_line r;
    r.set("workload", std::string("W6"));
    r.set("variant", std::string(slot) + "/" + std::to_string(slots) + "/" + std::to_string(line));
    r.set("elem", std::string("64"));
    r.set("slot", std::string(slot));
    r.set("slots", static_cast<std::uint64_t>(slots));
    r.set("line", static_cast<std::uint64_t>(line));
    r.set("rep", c.rep);
    r.set("mops", mops);
    r.set("tsc_hz", c.tsc_hz);
    r.write(c.json);
    std::fprintf(stderr, "W6 %-10s slots %-6u line %-4u %8.2f Mops/s\n", slot, slots, line, mops);
}

auto make_inline = [](std::uint64_t i) { E64 e{}; e.seq = i; return e; };
auto read_inline = [](const E64& e) { return e.seq; };
auto make_shared = [](std::uint64_t i) { auto p = std::make_shared<E64>(); p->seq = i; return p; };
auto read_shared = [](const std::shared_ptr<E64>& p) { return p->seq; };

} // namespace

int main(int argc, char** argv) {
    args a(argc, argv);
    config c;
    c.json = a.get("json", "");
    c.n = a.getu("n", 20'000'000);
    c.rep = static_cast<int>(a.getu("rep", 0));
    c.cpus = choose_cpus(a, 2);
    c.tsc_hz = calibrate_tsc_hz();

    const int cfg = static_cast<int>(a.getu("config", 0));
    if (a.has("list")) { std::printf("10\n"); return 0; }

    switch (cfg) {
    case 0: report(c, "inline", 64, 64, (run_one<E64, 64, 64>(c, make_inline, read_inline))); break;
    case 1: report(c, "inline", 256, 64, (run_one<E64, 256, 64>(c, make_inline, read_inline))); break;
    case 2: report(c, "inline", 1024, 64, (run_one<E64, 1024, 64>(c, make_inline, read_inline))); break;
    case 3: report(c, "inline", 4096, 64, (run_one<E64, 4096, 64>(c, make_inline, read_inline))); break;
    case 4: report(c, "inline", 16384, 64, (run_one<E64, 16384, 64>(c, make_inline, read_inline))); break;
    case 9: report(c, "inline", 2048, 64, (run_one<E64, 2048, 64>(c, make_inline, read_inline))); break;
    case 5: report(c, "inline", 256, 128, (run_one<E64, 256, 128>(c, make_inline, read_inline))); break;
    case 6: report(c, "inline", 4096, 128, (run_one<E64, 4096, 128>(c, make_inline, read_inline))); break;
    // With an allocation per element the shape stops mattering; two sizes
    // are enough to show that it is flat.
    case 7: report(c, "shared_ptr", 256, 64,
                   (run_one<std::shared_ptr<E64>, 256, 64>(c, make_shared, read_shared))); break;
    case 8: report(c, "shared_ptr", 4096, 64,
                   (run_one<std::shared_ptr<E64>, 4096, 64>(c, make_shared, read_shared))); break;
    default: std::fprintf(stderr, "--config must be 0..9\n"); return 2;
    }
    return 0;
}
