// The policy rule of a lifted wait (shim/lifted.hpp, lifted_wait_policy;
// docs/lifting.md, "The policy"), on affinity masks the test sets itself:
// counts not known always spin then park after the pause, whatever the
// mask; a paced producer too; one producer and at most four waiters
// spin (with a yield budget, so that an idle pool parks), unless the
// threads outnumber the mask, which parks after the pause; that branch
// counts only known threads.
#include "../shim/lifted.hpp"

#include <cstdint>
#include <cstdio>
#include <sched.h>
#include <thread>

namespace {

int failures = 0;

// yield: the yield budget expected, 0 for spinning not at all.
void expect(const char *what, wtc::lifted_wait_choice c, std::uint64_t yield) {
    if (c.policy != wtc::rt::wait_policy::spin_then_park || c.yield_ns != yield || c.spin_ns != wtc::rt::waiter::kSpinNs) {
        std::printf("FAIL %s: policy %d spin_ns %llu yield_ns %llu\n", what, static_cast<int>(c.policy),
                    static_cast<unsigned long long>(c.spin_ns), static_cast<unsigned long long>(c.yield_ns));
        ++failures;
    }
}

bool pin(unsigned n) {
    cpu_set_t all, want;
    CPU_ZERO(&all);
    CPU_ZERO(&want);
    if (sched_getaffinity(0, sizeof(all), &all) != 0) return false;
    unsigned got = 0;
    for (unsigned c = 0; c < CPU_SETSIZE && got < n; ++c)
        if (CPU_ISSET(c, &all)) CPU_SET(c, &want), ++got;
    return got == n && sched_setaffinity(0, sizeof(want), &want) == 0;
}

}  // namespace

int main() {
    using wtc::lifted_wait_policy;
    const unsigned cpus = wtc::rt::affinity_cpus();
    if (cpus == 0) {
        std::printf("FAIL the affinity mask cannot be read\n");
        return 1;
    }
    if (cpus >= 8) {
        expect("1 producer, 1 waiter", lifted_wait_policy(1, 1, false), wtc::kLiftedYieldNs);
        expect("1 producer, 4 waiters", lifted_wait_policy(1, 4, false), wtc::kLiftedYieldNs);
    }
    expect("paced", lifted_wait_policy(1, 2, true), 0);
    expect("producers not known", lifted_wait_policy(0, 2, false), 0);
    expect("waiters not known", lifted_wait_policy(1, 0, false), 0);
    expect("nothing known", lifted_wait_policy(0, 0, false), 0);
    if (cpus >= 16) expect("2 producers, 8 waiters on a wide mask", lifted_wait_policy(2, 8, false), 0);
    if (pin(1)) {
        expect("nothing known, one processor", lifted_wait_policy(0, 0, false), 0);
        expect("producers not known, one processor", lifted_wait_policy(0, 8, false), 0);
        expect("2 producers, 2 waiters, one processor", lifted_wait_policy(2, 2, false), 0);
        expect("1 producer, 1 waiter, one processor", lifted_wait_policy(1, 1, false), 0);
        expect("1 producer, 8 waiters, one processor", lifted_wait_policy(1, 8, false), 0);
    } else {
        std::printf("FAIL cannot set the affinity mask\n");
        ++failures;
    }
    if (failures) return 1;
    std::printf("policy ok\n");
    return 0;
}
