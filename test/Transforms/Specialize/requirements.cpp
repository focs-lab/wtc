// A queue whose operation sits on a path with a requirement of progress
// gets an implementation that meets it, when the runtime offers one. The
// runtime announces an implementation beyond the five every runtime has
// by a marker its queue's constructor calls with the tag that selects it,
// and for one built for a number of threads that number too. The queue
// types below stand for runtimes that announce what each case needs; the
// shim's unbounded queue announces jiffy.
//
// An implementation meets a requirement when each operation of the queue
// on a requiring path passes, under it, what the progress check holds
// that operation to: the level, no wait for data, no allocation, no
// first-use effect, and the bound that within k and progress-critical ask.
//
//   a bounded queue with one producer and one consumer gets spsc_settled
//   for a wait-free consumer, ahead of the rings of indices it also
//   offers; with many producers and one consumer, mpsc_settled for a
//   lock-free producer, and scq for a lock-free consumer, whose settled
//   pop still waits for the many producers; with many on both sides, scq
//   for a lock-free producer;
//   none of the others meets its requirement, and the remark says why for
//   each implementation tried: an unbounded queue's push may allocate,
//   jiffy's too, on the shim's queue as on another runtime's, under
//   wait-free and progress-critical alike; an unbounded queue with many
//   consumers has no implementation here; wcq takes a per-thread record on
//   first use; a ring built for two threads does not serve the four
//   proved, and a ring of elements whose destructor is the program's own
//   code is not chosen either, since that code could start another
//   operation on the same queue, which the bound counts apart; a pop
//   wait-free within 5 steps is not met by spsc_settled's 10; and a signal
//   handler's push to a bounded queue gets no implementation whose
//   consumer waits for a push in progress, nor one with a first-use
//   effect, nor a ring of indices for the any number of threads a handler
//   stands for.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --mlir-print-op-on-diagnostic=false -o %t.out.cir 2>&1 | FileCheck %s
// RUN: FileCheck %s --check-prefix=IR < %t.out.cir
//
// CHECK-DAG: remark: wtc: specialised: constructor tag rewritten to spsc_settled, for take_ss(int&), declared wait-free
// CHECK-DAG: remark: wtc: specialised: constructor tag rewritten to mpsc_settled, for put_mp(int), declared lock-free
// CHECK-DAG: remark: wtc: specialised: constructor tag rewritten to scq, for take_mp(int&), declared lock-free
// CHECK-DAG: remark: wtc: specialised: constructor tag rewritten to scq, for put_lf(int), declared lock-free
// CHECK-DAG: remark: wtc: no implementation meets a requirement: put_offered(), declared wait-free: mpsc: its push is lock-free; jiffy: its push may call the allocator
// CHECK-DAG: remark: wtc: no implementation meets a requirement: put_shim(), declared wait-free: mpsc: its push is lock-free; jiffy: its push may call the allocator
// CHECK-DAG: remark: wtc: no implementation meets a requirement: put_critical(), declared progress-critical: mpsc: its push is lock-free; jiffy: its push may call the allocator
// CHECK-DAG: remark: wtc: no implementation meets a requirement: take_many(int&), declared lock-free: generic: its pop transfer is blocking (starvation-free); an unbounded queue with many consumers has no other implementation here, and a capacity would allow a ring of indices
// CHECK-DAG: remark: wtc: no implementation meets a requirement: put_wf(int), declared wait-free: generic: its bounded try_push is blocking (starvation-free); scq: its bounded try_push is lock-free; wcq: its bounded try_push takes, on first use, a per-thread record
// CHECK-DAG: remark: wtc: no implementation meets a requirement: put_small(int), declared lock-free: generic: its bounded try_push is blocking (starvation-free); scq: it needs at most 2 threads operating on the queue, and the producers and consumers proved are 4; wcq: its bounded try_push takes, on first use, a per-thread record
// CHECK-DAG: remark: wtc: no implementation meets a requirement: put_tracked(Tracked), declared lock-free: generic: its bounded try_push is blocking (starvation-free); scq: it counts operations in flight, and the element's own Tracked::~Tracked() runs inside each, which may start another on the same queue; wcq: its bounded try_push takes, on first use, a per-thread record
// CHECK-DAG: remark: wtc: no implementation meets a requirement: take_within(int&), declared wait-free within 5 steps: spsc: its bounded pop transfer is blocking (starvation-free); spsc_settled: its bounded pop transfer may take 10 steps; scq: its bounded pop transfer is lock-free; wcq: its bounded pop transfer takes, on first use, a per-thread record
// CHECK-DAG: remark: wtc: no implementation meets a requirement: on_signal(int), a signal handler, installed by signal: mpsc: its bounded try_push leaves the consumer waiting for a push the handler may interrupt; mpsc_settled: its bounded try_push leaves the consumer waiting for a push the handler may interrupt; scq: it needs at most 128 threads operating on the queue, and the producers and consumers proved are unbounded; wcq: its bounded try_push takes, on first use, a per-thread record
//
// The tags are the runtime's own: 15 and 16 for the rings of indices, 18
// and 19 for the settled rings. Each operation carries its queue's choice.
// IR-LABEL: cir.func {{.*}}@_Z7take_ssRi(
// IR: wtc.queue_pop{{.*}}impl = #wtc.impl<spsc_settled>
// IR-LABEL: cir.func {{.*}}@_Z6put_mpi(
// IR: wtc.queue_push{{.*}}impl = #wtc.impl<mpsc_settled>
// IR-LABEL: cir.func {{.*}}@_Z7take_mpRi(
// IR: wtc.queue_pop{{.*}}impl = #wtc.impl<scq>
// IR-LABEL: cir.func {{.*}}@_Z6put_lfi(
// IR: wtc.queue_push{{.*}}impl = #wtc.impl<scq>

#include "progress.hpp"
#include "queue.hpp"
#include "thread.hpp"

#include <csignal>

namespace offering {

// A queue of a runtime that offers jiffy for an unbounded queue.
template <class T>
struct queue {
    [[clang::annotate("wtc_queue_impl_jiffy")]] static void offers_jiffy(int) {}
    [[clang::annotate("wtc_queue_construction")]] explicit queue(wtc::topology t = wtc::topology::generic) : tag(t) {
        offers_jiffy(17);
    }
    [[clang::annotate("wtc_queue_push")]] void push(const T &v) { last = v; }
    [[clang::annotate("wtc_queue_try_pop_into")]] bool try_pop(T &v) {
        v = last;
        return true;
    }
    wtc::topology tag;
    T last{};
};

// A bounded queue of a runtime that offers the rings of indices, built for
// Threads threads, and the settled rings.
template <class T, unsigned Threads = 128>
struct ring {
    [[clang::annotate("wtc_queue_impl_scq")]] static void offers_scq(int, unsigned) {}
    [[clang::annotate("wtc_queue_impl_wcq")]] static void offers_wcq(int, unsigned) {}
    [[clang::annotate("wtc_queue_impl_spsc_settled")]] static void offers_spsc_settled(int) {}
    [[clang::annotate("wtc_queue_impl_mpsc_settled")]] static void offers_mpsc_settled(int) {}
    [[clang::annotate("wtc_queue_construction")]] explicit ring(wtc::topology t = wtc::topology::generic) : tag(t) {
        offers_scq(15, Threads);
        offers_wcq(16, Threads);
        offers_spsc_settled(18);
        offers_mpsc_settled(19);
    }
    [[clang::annotate("wtc_queue_try_push"), clang::annotate("wtc_queue_bounded")]] bool try_push(const T &v) {
        last = v;
        return true;
    }
    [[clang::annotate("wtc_queue_try_pop_into"), clang::annotate("wtc_queue_bounded")]] bool try_pop(T &v) {
        v = last;
        return true;
    }
    wtc::topology tag;
    T last{};
};

} // namespace offering

// An element whose destructor is the program's own code.
struct Tracked {
    int v = 0;
    ~Tracked() { ++destroyed; }
    static int destroyed;
};
int Tracked::destroyed = 0;

offering::queue<int> offered, critical_q;
wtc::queue<int> shim_q;
wtc::queue<int> many;
offering::ring<int> ss_ring, mp_ring, mp_ring2, lf_ring, wf_ring, within_ring, signal_ring;
offering::ring<int, 2> small_ring;
offering::ring<Tracked> tracked_ring;

WTC_WAIT_FREE bool take_ss(int &v) { return ss_ring.try_pop(v); }
WTC_LOCK_FREE bool put_mp(int v) { return mp_ring.try_push(v); }
WTC_LOCK_FREE bool take_mp(int &v) { return mp_ring2.try_pop(v); }
WTC_LOCK_FREE bool put_lf(int v) { return lf_ring.try_push(v); }
WTC_WAIT_FREE void put_offered() { offered.push(1); }
WTC_WAIT_FREE void put_shim() { shim_q.push(1); }
WTC_PROGRESS_CRITICAL void put_critical() { critical_q.push(1); }
WTC_LOCK_FREE bool take_many(int &v) { return many.try_pop(v); }
WTC_WAIT_FREE bool put_wf(int v) { return wf_ring.try_push(v); }
WTC_LOCK_FREE bool put_small(int v) { return small_ring.try_push(v); }
WTC_LOCK_FREE bool put_tracked(Tracked t) { return tracked_ring.try_push(t); }
WTC_WAIT_FREE_WITHIN(5) bool take_within(int &v) { return within_ring.try_pop(v); }

void on_signal(int) { signal_ring.try_push(1); }

void producer() {
    put_offered();
    put_shim();
    put_critical();
    many.push(1);
    put_mp(1);
    mp_ring2.try_push(1);
    put_lf(1);
    put_wf(1);
    put_small(1);
    put_tracked(Tracked{});
}

void consumer_one() {
    int v = 0;
    offered.try_pop(v);
    critical_q.try_pop(v);
    shim_q.try_pop(v);
    mp_ring.try_pop(v);
    take_mp(v);
    signal_ring.try_pop(v);
}

void consumer_many() {
    int v = 0;
    take_many(v);
    lf_ring.try_pop(v);
    wf_ring.try_pop(v);
    small_ring.try_pop(v);
    Tracked t;
    tracked_ring.try_pop(t);
}

void ss_producer() {
    ss_ring.try_push(1);
    within_ring.try_push(1);
}
void ss_consumer() {
    int v = 0;
    take_ss(v);
    take_within(v);
}

int main() {
    signal(SIGINT, on_signal);
    std::thread a = wtc::spawn(producer), b = wtc::spawn(producer);
    std::thread c = wtc::spawn(consumer_one);
    std::thread d = wtc::spawn(consumer_many), e = wtc::spawn(consumer_many);
    std::thread f = wtc::spawn(ss_producer), g = wtc::spawn(ss_consumer);
    a.join();
    b.join();
    c.join();
    d.join();
    e.join();
    f.join();
    g.join();
}
