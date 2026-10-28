// The threshold depends on how many threads take the mutex, when the thread
// walk can count them (decisions/0006). Each loop here does the same work
// between sections, about 223 ns on the default class of host: over its
// 180 ns threshold for two threads, under the 275 ns one for four to eight,
// and no matter with one thread, which no other waits for. A count W8 did
// not measure (three, more than eight), threads started in a loop of
// unknown count, and a mutex that is not a global get the lowest threshold.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --wtc-batch-locks -o /dev/null 2>&1 | FileCheck %s

#include "mutex.hpp"
#include "progress.hpp"
#include "thread.hpp"

wtc::mutex alone, four, two, three, nine, many;
long a, b, c, d, e, f, g;

// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop batched: {{.*}}; work between sections estimated at 223 ns (1508 units), with one thread taking the mutex, which no other waits for
WTC_BATCH void by_one() { long x = 1; for (int i = 0; i < 1000; ++i) { alone.lock(); a += x; alone.unlock(); for (int k = 0; k < 150; ++k) x = x * 31 + k; } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop batched: {{.*}}; work between sections estimated at 223 ns (1508 units), under the 275 ns threshold for the 4 threads that take the mutex
WTC_BATCH void by_four() { long x = 1; for (int i = 0; i < 1000; ++i) { four.lock(); b += x; four.unlock(); for (int k = 0; k < 150; ++k) x = x * 31 + k; } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: work between sections estimated at 223 ns (1508 units), over the 180 ns threshold for the 2 threads that take the mutex
WTC_BATCH void by_two() { long x = 1; for (int i = 0; i < 1000; ++i) { two.lock(); c += x; two.unlock(); for (int k = 0; k < 150; ++k) x = x * 31 + k; } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: work between sections estimated at 223 ns (1508 units), over the 180 ns threshold that holds for any number of threads (3 threads take the mutex, a count not measured)
WTC_BATCH void by_three() { long x = 1; for (int i = 0; i < 1000; ++i) { three.lock(); d += x; three.unlock(); for (int k = 0; k < 150; ++k) x = x * 31 + k; } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: work between sections estimated at 223 ns (1508 units), over the 180 ns threshold that holds for any number of threads (9 threads take the mutex, a count not measured)
WTC_BATCH void by_nine() { long x = 1; for (int i = 0; i < 1000; ++i) { nine.lock(); e += x; nine.unlock(); for (int k = 0; k < 150; ++k) x = x * 31 + k; } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: work between sections estimated at 223 ns (1508 units), over the 180 ns threshold that holds for any number of threads (the threads that take the mutex are not counted here)
WTC_BATCH void by_many() { long x = 1; for (int i = 0; i < 1000; ++i) { many.lock(); f += x; many.unlock(); for (int k = 0; k < 150; ++k) x = x * 31 + k; } }
// A mutex that is not a global is not counted, even a local one that no
// other thread could take: the count names globals only.
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: work between sections estimated at 223 ns (1508 units), over the 180 ns threshold that holds for any number of threads (the threads that take the mutex are not counted here)
WTC_BATCH void by_local() { wtc::mutex mx; long x = 1; for (int i = 0; i < 1000; ++i) { mx.lock(); g += x; mx.unlock(); for (int k = 0; k < 150; ++k) x = x * 31 + k; } }

int main(int argc, char **) {
    std::thread t1 = wtc::spawn(by_one), r1 = wtc::spawn(by_local);
    std::thread f1 = wtc::spawn(by_four), f2 = wtc::spawn(by_four), f3 = wtc::spawn(by_four), f4 = wtc::spawn(by_four);
    std::thread w1 = wtc::spawn(by_two), w2 = wtc::spawn(by_two);
    std::thread h1 = wtc::spawn(by_three), h2 = wtc::spawn(by_three), h3 = wtc::spawn(by_three);
    std::thread n1 = wtc::spawn(by_nine), n2 = wtc::spawn(by_nine), n3 = wtc::spawn(by_nine), n4 = wtc::spawn(by_nine),
                n5 = wtc::spawn(by_nine), n6 = wtc::spawn(by_nine), n7 = wtc::spawn(by_nine), n8 = wtc::spawn(by_nine),
                n9 = wtc::spawn(by_nine);
    std::thread pool[8];
    for (int i = 0; i < argc && i < 8; ++i) pool[i] = wtc::spawn(by_many);
    t1.join(); r1.join(); f1.join(); f2.join(); f3.join(); f4.join(); w1.join(); w2.join();
    h1.join(); h2.join(); h3.join();
    n1.join(); n2.join(); n3.join(); n4.join(); n5.join(); n6.join(); n7.join(); n8.join(); n9.join();
    for (int i = 0; i < argc && i < 8; ++i) pool[i].join();
    return 0;
}
