// The queue is built one call below the function whose loop starts its
// consumers, and that function runs twice: pool(3), then pool(1). The
// second run builds the queue again into the same global, and the first
// run's three workers go on popping from it. A tag chosen at the second
// build from the second run's count (one consumer: the SPSC ring) would be
// wrong, and WTC_CHECK would abort on the second consumer; the count is
// credited to the loop only when the constructor ran once, in the one call
// the loop's function makes to build the queue, so the static verdict
// stands. The same with the queue built in the loop's function itself
// (SAME). Built by a pool that runs once (ONCE), the tag is chosen when
// the queue is built, as for a constructor in the loop's own function.
//
// RUN: %wtc_cc -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %wtc_cc -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=NONE
// RUN: %t.exe 3 | FileCheck %s --check-prefix=OUT
// RUN: %t.exe 1 | FileCheck %s --check-prefix=OUT
// RUN: %wtc_cc -DWTC_CHECK -DSAME %s -o %t.same.exe 2>&1 | FileCheck %s --check-prefix=NONE
// RUN: %t.same.exe 3 | FileCheck %s --check-prefix=OUT
// RUN: %wtc_cc -DWTC_CHECK -DONCE %s -o %t.once.exe 2>&1 | FileCheck %s --check-prefix=ONCE
// RUN: %t.once.exe 1 | FileCheck %s --check-prefix=OUT
// RUN: %t.once.exe 3 | FileCheck %s --check-prefix=OUT
//
// CHECK: queue *@G: spmc (producers: 1, consumers: many)
// NONE-NOT: specialised at run time
// ONCE: specialised at run time: {{.*}} when the loop at {{.*}} (consumers) runs at most once, else spmc; chosen at the call at {{.*}}, which builds it
// OUT: got 200000

#include "queue.hpp"
#include "thread.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

wtc::queue<int> *G;
std::atomic<int> done{0};
std::atomic<long> got{0};
std::vector<std::thread> ts;

void work() {
    while (!done.load()) {
        if (G->try_pop()) got.fetch_add(1);
    }
}
#ifdef SAME
#define BUILD() (G = new wtc::queue<int>())
#else
void build() { G = new wtc::queue<int>(); }
#define BUILD() build()
#endif
void pool(int n) {
    BUILD();
    for (int i = 0; i < n; i++) ts.push_back(wtc::spawn(work));
}
int main(int argc, char **argv) {
    int first = argc > 1 ? atoi(argv[1]) : 3;
    pool(first);
#ifndef ONCE
    pool(1);
#endif
    for (int i = 0; i < 200000; ++i) G->push(i);
    while (got.load() < 200000) {}
    done.store(1);
    for (auto &t : ts) t.join();
    std::printf("got %ld\n", got.load());
    return 0;
}
