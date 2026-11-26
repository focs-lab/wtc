// WB: the shim's bounded queue (shim/bounded.hpp) through W9's own
// measuring code, unchanged: this file includes w9_mpmc_scaling.cpp with
// its main renamed and adds the bounded queue's variants.
//
//   wb_bounded --variant bounded_generic --mode split --producers 4 --consumers 4
//   wb_bounded --variant bounded_scq --mode pairwise --threads 8
//
// Variants, each wtc::bounded<E> of capacity 8192, not overrunning, spin
// policy (the eventcounts are notified but never waited on: a full push
// and an empty pop spin in the workload, with W9's pause):
//
//   bounded_spsc      tag spsc: the bounded ring, both sides single
//   bounded_mpsc      tag mpsc: the ring, many producers, one consumer
//   bounded_spmc      tag spmc: the ring, one producer, many consumers
//   bounded_generic   tag generic: the ring, many on both sides
//   bounded_scq       tag scq: the lock-free ring of indices (exact variant)
//   bounded_wcq       tag wcq: the wait-free ring of indices (exact variant)
//
// scq and wcq exist only where shim/topology.hpp has them (WB_HAVE_INDEX,
// set by the build for trees that do). A push that finds the queue full
// retries; W9's in-flight throttle (8192 elements) still applies.

#define main w9_main
#include "w9_mpmc_scaling.cpp"
#undef main

#include "bounded.hpp"

#if defined(WB_HAVE_INDEX)
static_assert(wtc::rt::kCas2WaitFree, "bounded_wcq must run the wait-free ring: build with cx16");
#endif

namespace {

constexpr std::size_t kWbCapacity = 8192;

template <class T, wtc::topology Tag>
struct bq {
    wtc::bounded<T> q{Tag, kWbCapacity, false, wtc::rt::wait_policy::spin};
    void push(const T& v) {
        while (!q.try_push(v)) cpu_relax();
    }
    bool try_pop(T& out) { return q.try_pop(out); }
};

template <class E>
bool with_bounded(const std::string& name, runner<E> f) {
    using wtc::topology;
    if (name == "bounded_spsc")    { f.template run<bq<E, topology::spsc>>(); return true; }
    if (name == "bounded_mpsc")    { f.template run<bq<E, topology::mpsc>>(); return true; }
    if (name == "bounded_spmc")    { f.template run<bq<E, topology::spmc>>(); return true; }
    if (name == "bounded_generic") { f.template run<bq<E, topology::generic>>(); return true; }
#if defined(WB_HAVE_INDEX)
    if (name == "bounded_scq")     { f.template run<bq<E, topology::scq>>(); return true; }
    if (name == "bounded_wcq")     { f.template run<bq<E, topology::wcq>>(); return true; }
#endif
    return false;
}

} // namespace

int main(int argc, char** argv) {
    args a(argc, argv);
    const std::string v = a.get("variant", "");
    if (v.rfind("bounded_", 0) != 0) return w9_main(argc, argv);
    config c;
    c.variant = v;
    c.mode = a.get("mode", "split");
    c.elem = a.get("elem", "8");
    c.wait = a.get("wait", "spin");
    c.json = a.get("json", "");
    c.n = a.getu("n", 2'000'000);
    c.producers = static_cast<int>(a.getu("producers", 1));
    c.consumers = static_cast<int>(a.getu("consumers", 1));
    c.threads = static_cast<int>(a.getu("threads", 2));
    c.rep = static_cast<int>(a.getu("rep", 0));
    if (c.mode != "split" && c.mode != "pairwise") { std::fprintf(stderr, "--mode is split or pairwise\n"); return 2; }
    if (c.producers < 1 || c.consumers < 1 || c.threads < 1 || c.producers > 255) { std::fprintf(stderr, "bad thread counts\n"); return 2; }
    const bool split = c.mode == "split";
    if (v == "bounded_spsc" && !(split && c.producers == 1 && c.consumers == 1)) { std::fprintf(stderr, "%s: split 1:1 only\n", v.c_str()); return 2; }
    if (v == "bounded_mpsc" && !(split && c.consumers == 1)) { std::fprintf(stderr, "%s: split, one consumer\n", v.c_str()); return 2; }
    if (v == "bounded_spmc" && !(split && c.producers == 1)) { std::fprintf(stderr, "%s: split, one producer\n", v.c_str()); return 2; }
    c.cpus = choose_cpus(a, split ? c.producers + c.consumers : c.threads);
    c.tsc_hz = calibrate_tsc_hz();
    bool ok = false;
    if (c.elem == "8") ok = with_bounded<E8>(v, runner<E8>(c));
    else if (c.elem == "64") ok = with_bounded<E64>(v, runner<E64>(c));
    else { std::fprintf(stderr, "--elem must be 8 or 64\n"); return 2; }
    if (!ok) { std::fprintf(stderr, "unknown variant %s\n", v.c_str()); return 2; }
    return 0;
}
