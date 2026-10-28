#!/usr/bin/env python3
# SPDX-License-Identifier: Zlib
# Derived from ThreadPool (fetched by fetch.sh beside this script), zlib
# licence; see LICENSE.
"""Makes the port's ThreadPool.h out of the library's: a copy in which the
pool's task queue is wtc::queue instead of std::queue under a mutex and a
condition variable, and nothing else changes.

    port.py <threadpool-src> <out-dir>              the port
    port.py <threadpool-src> <out-dir> --orig-spin  the control

What the port changes, exactly (every line is marked "WTC port"):
  - ThreadPool.h includes wtc_q.h (beside this script, copied next to it);
  - the member `tasks` is a wtc::queue<std::function<void()>>, built with
    THREADPOOL_WTC_TAG, which is empty unless the hand-tagged build
    defines it; the mutex and the condition variable go, and `stop` is a
    std::atomic<bool>, since the workers now read it without a mutex;
  - a worker takes a task with wtc_pool_pop (wtc_q.h), which spins, then
    yields, until it has one, and returns false once the pool is stopping
    and no task is left, where the original slept on the condition
    variable under the mutex until the same condition;
  - enqueue checks `stop` and pushes, with no mutex and no notification;
  - the destructor sets `stop` and joins, as before.

The constructor is the library's: the workers are started in a loop over
its argument, and the queue is a member built by the same constructor
before the loop, which is what lets the compiler choose the queue when the
pool is built (decisions/0005). The task, the packaged_task behind it and
the future are the library's too.

The control (--orig-spin) keeps the library's queue, mutex and condition
variable and changes only how a worker waits: instead of sleeping on the
condition variable, it releases the mutex and spins, then yields, on a
count the pool publishes under the mutex (tasks queued, plus one once the
pool is stopping), then takes the mutex and checks again, as the port's
workers spin and yield. The original against the control is the cost of
sleeping; the control against the port's general-purpose build is the
queue under a mutex against the lock-free one, both waiting by spinning.
The published count is one more shared line the port does not have.
"""
import os
import shutil
import sys

src, out = sys.argv[1], sys.argv[2]
spin = "--orig-spin" in sys.argv[3:]
here = os.path.dirname(os.path.abspath(__file__))

if os.path.exists(out):
    shutil.rmtree(out)
os.makedirs(out)
s = open(os.path.join(src, "ThreadPool.h")).read()


def patch(pairs):
    global s
    for old, new in pairs:
        n = s.count(old)
        assert n == 1, f"ThreadPool.h: expected one of {old!r}, found {n}"
        s = s.replace(old, new)


WORKER = """\
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock,
                            [this]{ return this->stop || !this->tasks.empty(); });
                        if(this->stop && this->tasks.empty())
                            return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
"""
ENQUEUE = """\
    {
        std::unique_lock<std::mutex> lock(queue_mutex);

        // don't allow enqueueing after stopping the pool
        if(stop)
            throw std::runtime_error("enqueue on stopped ThreadPool");

        tasks.emplace([task](){ (*task)(); });
    }
    condition.notify_one();
"""
DESTROY = """\
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;
    }
    condition.notify_all();
"""

if not spin:
    patch([
        ("#define THREAD_POOL_H\n",
         "#define THREAD_POOL_H\n\n"
         "// Altered by the WTC port (benchmarks/apps/threadpool/port.py): the task\n"
         "// queue is wtc::queue; every changed line is marked \"WTC port\".\n"),
        ("#include <stdexcept>\n",
         "#include <stdexcept>\n#include <atomic>\n\n#include \"wtc_q.h\"  // WTC port\n"),
        ("    std::queue< std::function<void()> > tasks;\n",
         "    wtc::queue< std::function<void()> > tasks{THREADPOOL_WTC_TAG};  // WTC port: the compiler rewrites the tag\n"),
        ("    // synchronization\n    std::mutex queue_mutex;\n    std::condition_variable condition;\n    bool stop;\n",
         "    // synchronization: the queue's own (WTC port)\n    std::atomic<bool> stop;  // WTC port\n"),
        (WORKER,
         "                    // WTC port: spin, then yield, until a task, or the stop and none left\n"
         "                    if(!wtc_pool_pop(this->tasks, this->stop, task))\n"
         "                        return;\n"),
        (ENQUEUE,
         "    // don't allow enqueueing after stopping the pool\n"
         "    if(stop.load(std::memory_order_relaxed))  // WTC port\n"
         "        throw std::runtime_error(\"enqueue on stopped ThreadPool\");\n\n"
         "    tasks.push([task](){ (*task)(); });  // WTC port: no mutex, no notification\n"),
        (DESTROY, "    stop.store(true, std::memory_order_release);  // WTC port\n"),
    ])
else:
    patch([
        ("#define THREAD_POOL_H\n",
         "#define THREAD_POOL_H\n\n"
         "// Altered by the WTC port's control (benchmarks/apps/threadpool/port.py\n"
         "// --orig-spin): workers spin on a published count instead of sleeping;\n"
         "// every changed line is marked \"WTC control\".\n"),
        ("#include <stdexcept>\n",
         "#include <stdexcept>\n#include <atomic>\n\n"
         "#define THREADPOOL_WTC_CONTROL  // WTC control: wtc_q.h without the shim\n"
         "#include \"wtc_q.h\"  // WTC control\n"),
        ("    bool stop;\n",
         "    bool stop;\n"
         "    std::atomic<std::size_t> ready{0};  // WTC control: tasks queued, plus one once stopping\n"),
        ("                        this->condition.wait(lock,\n"
         "                            [this]{ return this->stop || !this->tasks.empty(); });\n",
         "                        wtc_spin_wait(lock, this->ready,  // WTC control\n"
         "                            [this]{ return this->stop || !this->tasks.empty(); });\n"),
        ("                        this->tasks.pop();\n",
         "                        this->tasks.pop();\n"
         "                        this->ready.store(this->tasks.size() + this->stop, std::memory_order_release);  // WTC control\n"),
        ("        tasks.emplace([task](){ (*task)(); });\n",
         "        tasks.emplace([task](){ (*task)(); });\n"
         "        ready.store(tasks.size() + stop, std::memory_order_release);  // WTC control\n"),
        ("        stop = true;\n",
         "        stop = true;\n"
         "        ready.store(tasks.size() + stop, std::memory_order_release);  // WTC control\n"),
    ])
shutil.copy(os.path.join(here, "wtc_q.h"), os.path.join(out, "wtc_q.h"))
open(os.path.join(out, "ThreadPool.h"), "w").write(s)
print(f"port: {out}/ThreadPool.h{' (control)' if spin else ''}")
