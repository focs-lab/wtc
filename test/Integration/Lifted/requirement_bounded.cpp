// A path's progress requirement chooses the bounded queue's implementation
// end to end, on the shim's own runtime. The program's ring under a mutex
// (spdlog's circular_q) is lifted to wtc::bounded, and a producer's step
// that offers an element is declared wait-free (lock-free with -DLF). The
// topology alone gives the bounded ring, whose try_push may wait for the
// pop that last held its cell, so the specialisation pass looks among
// what the runtime announces for the cheapest implementation that meets
// the requirement under the counts proved:
//
//   1 producer, 1 consumer, wait-free    spsc_settled: its single sides
//       never wait for each other, and it needs no bound on threads, so
//       it is preferred to the rings of indices; the check passes
//   2 producers, 1 consumer, lock-free   mpsc_settled: its push waits for
//       no pop, and its producers' compare-and-swap fails only to each
//       other's
//   2 producers, 2 consumers, lock-free  scq: no settled ring serves many
//       on both sides, and the four threads proved are within the 128
//       the shim builds it for
//   n producers, 2 consumers, lock-free  n read from the command line, a
//       count with no number: scq cannot be shown within its bound, so
//       nothing is chosen, a remark says why, and the declared requirement
//       fails the build
//   2 producers, 1 consumer, wait-free   the settled push is only lock-free
//       with two, scq is lock-free, and wcq takes a per-thread record on a
//       thread's first operation, which the check counts as blocking (it is
//       not announced here either: the shim announces it only where the
//       double-width compare-and-swap is one instruction, -mcx16, which
//       ClangIR cannot yet compile); the remark says why for each, and the
//       build fails
//
// The tag each build passes is read back from the LLVM IR it is compiled
// from (wtc-cc --keep): the lifted constructor gets the shim's number for
// the implementation chosen (shim/topology.hpp: spsc_settled 8,
// mpsc_settled 9, scq 5), which the shim's bounded queue maps to it
// (shim/bounded.hpp, kindOf and settled). Every element arrives once, in
// each build that runs, and again with WTC_CHECK, which holds each side the
// tag declares single to one thread.
//
// RUN: %wtc_cc --lift -O1 -I%S/../../Transforms/LiftLockedQueues/Inputs --keep %t.one.d %s -o %t.one.exe 2>&1 | FileCheck %s --check-prefix=ONE
// RUN: FileCheck %s --check-prefix=ONE-IR < %t.one.d/requirement_bounded.ll
// RUN: %t.one.exe | FileCheck %s --check-prefix=OUT
// RUN: %wtc_cc --lift -O1 -I%S/../../Transforms/LiftLockedQueues/Inputs -DWTC_CHECK %s -o %t.one.check.exe
// RUN: %t.one.check.exe | FileCheck %s --check-prefix=OUT
// RUN: %wtc_cc --lift -O1 -I%S/../../Transforms/LiftLockedQueues/Inputs -DTWO -DLF --keep %t.two.d %s -o %t.two.exe 2>&1 | FileCheck %s --check-prefix=TWO
// RUN: FileCheck %s --check-prefix=TWO-IR < %t.two.d/requirement_bounded.ll
// RUN: %t.two.exe | FileCheck %s --check-prefix=OUT
// RUN: %wtc_cc --lift -O1 -I%S/../../Transforms/LiftLockedQueues/Inputs -DTWO -DLF -DWTC_CHECK %s -o %t.two.check.exe
// RUN: %t.two.check.exe | FileCheck %s --check-prefix=OUT
// RUN: %wtc_cc --lift -O1 -I%S/../../Transforms/LiftLockedQueues/Inputs -DTWO -DTWO_CONSUMERS -DLF --keep %t.mpmc.d %s -o %t.mpmc.exe 2>&1 | FileCheck %s --check-prefix=MPMC
// RUN: FileCheck %s --check-prefix=MPMC-IR < %t.mpmc.d/requirement_bounded.ll
// RUN: %t.mpmc.exe | FileCheck %s --check-prefix=OUT
// RUN: %wtc_cc --lift -O1 -I%S/../../Transforms/LiftLockedQueues/Inputs -DTWO -DTWO_CONSUMERS -DLF -DWTC_CHECK %s -o %t.mpmc.check.exe
// RUN: %t.mpmc.check.exe | FileCheck %s --check-prefix=OUT
// RUN: not %wtc_cc --lift -O1 -I%S/../../Transforms/LiftLockedQueues/Inputs -DMANY -DTWO_CONSUMERS -DLF %s -o %t.many.exe 2>&1 | FileCheck %s --check-prefix=MANY
// RUN: not %wtc_cc --lift -O1 -I%S/../../Transforms/LiftLockedQueues/Inputs -DTWO %s -o %t.wf.exe 2>&1 | FileCheck %s --check-prefix=WF
//
// ONE: locked queue q_ in Box lifted (
// ONE: (producers: 1, consumers: 1)
// ONE: remark: wtc: specialised: constructor tag rewritten to spsc
// ONE: remark: wtc: specialised: constructor tag rewritten to spsc_settled, for produce(int), declared wait-free
// ONE: remark: wtc: produce(int) is declared wait-free: it is
// ONE-NOT: error
// ONE-IR: {{call|invoke}} void @{{.*}}lifted_bounded{{.*}}9construct{{.*}}(ptr %{{[0-9]+}}, i32 8,
//
// TWO: (producers: many, consumers: 1)
// TWO: remark: wtc: specialised: constructor tag rewritten to mpsc_settled, for produce(int), declared lock-free
// TWO: remark: wtc: produce(int) is declared lock-free: nothing it reaches blocks
// TWO-NOT: error
// TWO-IR: {{call|invoke}} void @{{.*}}lifted_bounded{{.*}}9construct{{.*}}(ptr %{{[0-9]+}}, i32 9,
//
// MPMC: (producers: many, consumers: many)
// MPMC: remark: wtc: specialised: constructor tag rewritten to scq, for produce(int), declared lock-free
// MPMC: remark: wtc: produce(int) is declared lock-free: nothing it reaches blocks
// MPMC-NOT: error
// MPMC-IR: {{call|invoke}} void @{{.*}}lifted_bounded{{.*}}9construct{{.*}}(ptr %{{[0-9]+}}, i32 5,
//
// MANY: (producers: many, consumers: many)
// MANY: remark: wtc: no implementation meets a requirement: produce(int), declared lock-free: generic: its bounded try_push is blocking (starvation-free); scq: it needs at most 128 threads operating on the queue, and the producers and consumers proved are {{.*}}; wcq: its bounded try_push takes, on first use, a per-thread record
// MANY-NOT: rewritten to scq
// MANY: error: wtc: produce(int) is declared lock-free but can block
//
// WF: (producers: many, consumers: 1)
// WF: remark: wtc: no implementation meets a requirement: produce(int), declared wait-free: mpsc: its bounded try_push is blocking (starvation-free); mpsc_settled: its bounded try_push is lock-free; scq: its bounded try_push is lock-free; wcq: its bounded try_push takes, on first use, a per-thread record
// WF-NOT: rewritten to
// WF: error: wtc: produce(int) is declared wait-free but can block
//
// OUT: sum 20000100000 discards 0

#include "circular_q.h"
#include "progress.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

class Box {
public:
    explicit Box(size_t n) : q_(n) {}
    void offer(int &&v) {
        bool pushed = false;
        {
            std::unique_lock<std::mutex> l(m_);
            if (!q_.full()) {
                q_.push_back(std::move(v));
                pushed = true;
            }
        }
        if (pushed) data_.notify_one();
        else ++discards_;
    }
    void pop(int &out) {
        {
            std::unique_lock<std::mutex> l(m_);
            data_.wait(l, [this] { return !q_.empty(); });
            out = std::move(q_.front());
            q_.pop_front();
        }
        space_.notify_one();
    }

private:
    std::mutex m_;
    std::condition_variable data_, space_;
    Ring<int> q_;

public:
    std::atomic<long> discards_{0};
};

// Room for every element, so that no offer is declined and the sum is
// known.
static const int kElements = 200000;
static Box box(kElements);

#if defined(LF)
WTC_LOCK_FREE
#else
WTC_WAIT_FREE
#endif
void produce(int v) { box.offer(std::move(v)); }

static void produce_range(int from, int to) {
    for (int i = from; i <= to; ++i) produce(i);
}

static void producer_all() { produce_range(1, kElements); }
static void producer_low() { produce_range(1, kElements / 2); }
static void producer_high() { produce_range(kElements / 2 + 1, kElements); }

static std::atomic<long long> sum{0};

static void consume(int count) {
    long long mine = 0;
    for (int i = 0; i < count; ++i) {
        int v = 0;
        box.pop(v);
        mine += v;
    }
    sum += mine;
}

#if defined(TWO_CONSUMERS)
static void consumer_one() { consume(kElements / 2); }
static void consumer_two() { consume(kElements - kElements / 2); }
#else
static void consumer() { consume(kElements); }
#endif

#if defined(MANY)
static int n_producers = 2;
static void producer_share() {
    static std::atomic<int> next{0};
    const int i = next.fetch_add(1);
    produce_range(i * kElements / n_producers + 1, (i + 1) * kElements / n_producers);
}
#endif

int main(int argc, char **argv) {
#if defined(TWO_CONSUMERS)
    std::thread c(consumer_one), d(consumer_two);
#else
    std::thread c(consumer);
#endif
#if defined(MANY)
    if (argc > 1) n_producers = std::atoi(argv[1]);
    std::vector<std::thread> producers;
    for (int i = 0; i < n_producers; ++i) producers.emplace_back(producer_share);
    for (std::thread &t : producers) t.join();
#elif defined(TWO)
    std::thread a(producer_low), b(producer_high);
    a.join();
    b.join();
#else
    std::thread p(producer_all);
    p.join();
#endif
    c.join();
#if defined(TWO_CONSUMERS)
    d.join();
#endif
    std::printf("sum %lld discards %ld\n", sum.load(), box.discards_.load());
    (void)argc;
    (void)argv;
}
