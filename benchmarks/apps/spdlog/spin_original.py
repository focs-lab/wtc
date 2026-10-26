# SPDX-License-Identifier: MIT
"""Copy spdlog's include/ and make its queue wait the way the port does.

    spin_original.py <spdlog-dir> <out-include-dir>

The control build spdlog-orig-spin: spdlog as released, with one change in
details/mpmc_blocking_q.h. Where the queue sleeps on a condition variable
(a full queue for a producer, an empty one for a worker), it releases the
mutex and spins, then yields, as the port's pop does (wtc_q.h), and takes
the mutex again. The mutex, the ring under it and every other line stay.
Between spdlog-orig and this build the difference is the waiting alone;
between this build and spdlog-generic it is the queue alone.
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
// spin_original.py: wait by spinning, then yielding, with the mutex released.
template <typename Lock, typename Pred>
inline void spin_wait(Lock &lock, Pred pred) {
    for (unsigned spins = 0; !pred(); ++spins) {
        lock.unlock();
        if (spins < 1024) {
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#endif
        } else {
            std::this_thread::yield();
        }
        lock.lock();
    }
}
template <typename Lock, typename Dur, typename Pred>
inline bool spin_wait_for(Lock &lock, Dur wait, Pred pred) {
    const auto deadline = std::chrono::steady_clock::now() + wait;
    for (unsigned spins = 0; !pred(); ++spins) {
        if (std::chrono::steady_clock::now() >= deadline) return pred();
        lock.unlock();
        if (spins < 1024) {
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#endif
        } else {
            std::this_thread::yield();
        }
        lock.lock();
    }
    return true;
}
"""

anchor = "namespace spdlog {\nnamespace details {\n"
assert text.count(anchor) == 1, "namespace anchor not found"
text = text.replace(anchor, anchor + helpers, 1)

replacements = [
    ("pop_cv_.wait(lock, ", "spin_wait(lock, "),
    ("push_cv_.wait(lock, ", "spin_wait(lock, "),
    ("push_cv_.wait_for(lock, wait_duration, ", "spin_wait_for(lock, wait_duration, "),
]
for old, new in replacements:
    n = text.count(old)
    assert n >= 1, f"no occurrence of {old!r}"
    text = text.replace(old, new)
assert ".wait(" not in text and ".wait_for(" not in text, "a condition-variable wait is left"
if "#include <thread>" not in text:
    text = text.replace("#include <mutex>", "#include <mutex>\n#include <thread>", 1)
q.write_text(text)
