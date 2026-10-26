// A program built with wtc-cc --census and run under the census library
// itself, built here from its source: the report counts the control blocks
// by the threads that touched them, the thread that made a block among
// them; and a child the program forks reports to a file of its own,
// <WTC_CENSUS_OUT>.<pid>, instead of overwriting the parent's.
//
// p is made by the main thread and copied by a second, so it has two
// threads; q is made and copied by the main thread only. Each gets 202
// operations: the making, 100 copies and their releases, and the last
// release. The child makes r and copies it five times, and exits with p
// and q still alive in its copy of the parent's memory.
//
// RUN: %clang -shared -fPIC -O2 %wtc_src/tools/census/threadcensus.c -o %t.so -ldl -lpthread
// RUN: %wtc_cc --census %s -o %t.exe
// RUN: rm -f %t.census %t.census.*
// RUN: env LD_PRELOAD=%t.so WTC_CENSUS_OUT=%t.census %t.exe
// RUN: FileCheck %s < %t.census
// RUN: cat %t.census.* | FileCheck %s --check-prefix=CHILD
//
// CHECK: reference counts: 2 control blocks made, 200 copies, 202 releases
// CHECK: threadcensus: 2 control blocks, 404 count operations
// CHECK: single-thread control blocks: 1 of 2; their count operations: 202 of 404 (50.0%)
//
// CHILD: reference counts: 3 control blocks made
// CHILD: single-thread control blocks: 2 of 3; their count operations: 212 of 413

#include <cstdlib>
#include <memory>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

static void copies(const std::shared_ptr<int>& p, int n) {
    for (int i = 0; i < n; ++i) {
        std::shared_ptr<int> c = p;
        (void)c;
    }
}

int main() {
    auto p = std::make_shared<int>(1);
    auto q = std::make_shared<int>(2);
    std::thread t([&p] { copies(p, 100); });
    t.join();
    copies(q, 100);
    pid_t child = fork();
    if (child == 0) {
        auto r = std::make_shared<int>(3);
        copies(r, 5);
        std::exit(0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
}
