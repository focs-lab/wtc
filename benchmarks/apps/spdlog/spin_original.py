# SPDX-License-Identifier: MIT
"""Copy spdlog's include/ and make its queue wait the way the port does.

    spin_original.py <spdlog-dir> <out-include-dir>

The control build spdlog-orig-spin: spdlog as released, with one change in
details/mpmc_blocking_q.h. Where the queue sleeps on a condition variable
(a full queue for a producer, an empty one for a worker), the thread
releases the mutex and spins on a size the queue publishes with every push
and pop, without touching the mutex, until the size says it can go on;
then it takes the mutex and checks again. A worker spins, then yields, as
the port's pop does; a producer only spins, as the port's bounded push
does (wtc_q.h). The mutex, the ring under it and every other line stay.

What the control isolates: spdlog-orig against it is the cost of sleeping
on condition variables; it against spdlog-generic is the queue, mutex and
ring against the port's lock-free queue, both waiting by spinning. What it
cannot isolate: the size it spins on is one more shared line the port
does not have, and its waiters still take the mutex once per wake-up.
"""
import pathlib
import shutil
import sys

src = pathlib.Path(sys.argv[1]) / "include"
out = pathlib.Path(sys.argv[2])
if out.exists():
    shutil.rmtree(out)
shutil.copytree(src, out)
q = out / "spdlog" / "details" / "mpmc_blocking_q.h"
text = q.read_text()

helpers = """
// spin_original.py: wait by spinning on the published size, mutex released.
inline void spin_relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}
// A producer facing a full queue: spin only, as the port's bounded push.
template <typename Lock, typename Pred, typename Ready>
inline void spin_wait_push(Lock &lock, Pred pred, Ready ready) {
    while (!pred()) {
        lock.unlock();
        while (!ready()) spin_relax();
        lock.lock();
    }
}
// A worker facing an empty queue: spin, then yield, as the port's pop.
template <typename Lock, typename Pred, typename Ready>
inline void spin_wait_pop(Lock &lock, Pred pred, Ready ready) {
    while (!pred()) {
        lock.unlock();
        for (unsigned spins = 0; !ready(); ++spins) {
            if (spins < 1024) spin_relax();
            else std::this_thread::yield();
        }
        lock.lock();
    }
}
// The same with a deadline; returns pred() as condition_variable::wait_for.
template <typename Lock, typename Dur, typename Pred, typename Ready>
inline bool spin_wait_pop_for(Lock &lock, Dur wait, Pred pred, Ready ready) {
    const auto deadline = std::chrono::steady_clock::now() + wait;
    while (!pred()) {
        // On a later pass: the size said go on, but another waiter took the
        // element first.
        if (std::chrono::steady_clock::now() >= deadline) return false;
        lock.unlock();
        for (unsigned spins = 0; !ready(); ++spins) {
            // Every 256 spins while pausing, every time once yielding: a
            // yield can take long enough to overrun a short timeout.
            if ((spins >= 1024 || (spins & 255) == 255) &&
                std::chrono::steady_clock::now() >= deadline) {
                lock.lock();
                return pred();
            }
            if (spins < 1024) spin_relax();
            else std::this_thread::yield();
        }
        lock.lock();
    }
    return true;
}
"""


def replace_all(old, new, at_least=1):
    global text
    n = text.count(old)
    assert n >= at_least, f"expected {old!r} at least {at_least} time(s), found {n}"
    text = text.replace(old, new)


anchor = "namespace spdlog {\nnamespace details {\n"
assert text.count(anchor) == 1, "namespace anchor not found"
text = text.replace(anchor, anchor + helpers, 1)
if "#include <thread>" not in text:
    replace_all("#include <mutex>", "#include <mutex>\n#include <thread>")
if "#include <chrono>" not in text:
    replace_all("#include <mutex>", "#include <chrono>\n#include <mutex>")

# The published size and the capacity it is compared with.
replace_all("        : q_(max_items) {}", "        : q_(max_items), spin_max_(max_items) {}")
replace_all("    spdlog::details::circular_q<T> q_;\n",
            "    spdlog::details::circular_q<T> q_;\n"
            "    std::atomic<size_t> spin_size_{0};\n"
            "    size_t spin_max_;\n")
replace_all("q_.push_back(std::move(item));",
            "q_.push_back(std::move(item));\n"
            "        spin_size_.store(q_.size(), std::memory_order_release);", at_least=6)
replace_all("q_.pop_front();",
            "q_.pop_front();\n"
            "        spin_size_.store(q_.size(), std::memory_order_release);", at_least=4)

ready_push = "[this] { return spin_size_.load(std::memory_order_acquire) < spin_max_; }"
ready_pop = "[this] { return spin_size_.load(std::memory_order_acquire) != 0; }"
replace_all("pop_cv_.wait(lock, [this] { return !this->q_.full(); });",
            "spin_wait_push(lock, [this] { return !this->q_.full(); }, " + ready_push + ");", at_least=2)
replace_all("push_cv_.wait(lock, [this] { return !this->q_.empty(); });",
            "spin_wait_pop(lock, [this] { return !this->q_.empty(); }, " + ready_pop + ");", at_least=2)
replace_all("push_cv_.wait_for(lock, wait_duration, [this] { return !this->q_.empty(); })",
            "spin_wait_pop_for(lock, wait_duration, [this] { return !this->q_.empty(); }, " + ready_pop + ")",
            at_least=2)
assert ".wait(" not in text and ".wait_for(" not in text, "a condition-variable wait is left"
q.write_text(text)
