#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Derived from BS::thread_pool (fetched by fetch.sh beside this script),
# MIT; see LICENSE.
"""Makes the port's BS_thread_pool.hpp out of the library's: a copy in which
the pool's task queue is wtc::queue instead of std::queue under the pool's
mutex, and nothing else changes that does not have to.

    port.py <thread-pool-src> <out-dir>              the port
    port.py <thread-pool-src> <out-dir> --orig-spin  the control

What the port changes, exactly (every changed line is marked "WTC port"):
  - wtc_q.h (beside this script, copied next to the header) is included
    after the standard headers;
  - the member `tasks` is a wtc::queue<task_t>, built with BS_WTC_TAG,
    which is empty unless the hand-tagged build defines it; beside it,
    `wtc_counts` (tasks unfinished, threads waiting, the stop) and a flag
    per worker replace `tasks_running`, `waiting` and `workers_running`;
  - detach_task and detach_bulk (through which submit_task and every bulk
    submission go) count the task and push it, with no mutex and no
    notification;
  - the worker counts its previous task (or its initialisation) finished,
    then pops with wtc_port::pop, which spins, then yields, until it has a
    task or the pool is stopping, where the original slept on a condition
    variable under the mutex until the same condition;
  - wait(), wait_for() and wait_until() wait on the same condition
    variable, under the same mutex, until nothing is unfinished;
  - get_tasks_queued(), get_tasks_running() and get_tasks_total() read the
    counters; purge() pops and drops; create_threads() counts the workers'
    initialisations and clears the stop; destroy_threads() sets the stop;
  - pop_task() goes, and a static_assert says that the pool with
    priorities or pausing is not ported.

What stays the library's: the constructors, create_threads() with its loop
over thread_count, submit_task() and the task_and_future that makes a
task and its promise, the futures, the init and cleanup functions, the
thread-local pool and index, the C++17 move_only_function, reset().

The control (--orig-spin) keeps the library's queue, mutex and counts and
changes only how a worker waits for a task: instead of sleeping on the
condition variable, it releases the mutex and spins, then yields, on a
count the pool publishes under the mutex (tasks queued, plus one once the
pool is stopping), then takes the mutex and checks again, as the port's
workers spin and yield (every changed line is marked "WTC control").
"""
import os
import shutil
import sys

src, out = sys.argv[1], sys.argv[2]
spin = "--orig-spin" in sys.argv[3:]
here = os.path.dirname(os.path.abspath(__file__))
NAME = "BS_thread_pool.hpp"

if os.path.exists(out):
    shutil.rmtree(out)
os.makedirs(out)
s = open(os.path.join(src, "include", NAME)).read()


def patch(old, new, count=1):
    global s
    n = s.count(old)
    assert n == count, f"{NAME}: expected {count} of {old!r}, found {n}"
    s = s.replace(old, new)


def cut(first, last, new):
    """Replaces the text from `first` through the next `last` with `new`."""
    global s
    assert s.count(first) == 1, f"{NAME}: expected one of {first!r}"
    a = s.index(first)
    b = s.index(last, a) + len(last)
    s = s[:a] + new + s[b:]


INCLUDE_AT = "#ifdef max\n    #undef max\n#endif\n"

DETACH_TASK = """\
        {
            const std::scoped_lock tasks_lock(tasks_mutex);
            if constexpr (priority_enabled)
                tasks.emplace(std::forward<F>(task), priority);
            else
                tasks.emplace(std::forward<F>(task));
        }
        task_available_cv.notify_one();
"""
DETACH_BULK = """\
        if (first != last)
        {
            bool notify = false;
            {
                const std::scoped_lock tasks_lock(tasks_mutex);
                if constexpr (pause_enabled)
                    notify = tasks.empty() && !paused;
                else
                    notify = tasks.empty();
                for (I it = first; it != last; ++it)
                {
                    if constexpr (priority_enabled)
                        tasks.emplace(std::move(*it), priority);
                    else
                        tasks.emplace(std::move(*it));
                }
            }
            if (notify)
                task_available_cv.notify_all();
        }
"""
WAIT_PREDICATE = """\
            [this]
            {
                if constexpr (pause_enabled)
                    return (tasks_running == 0) && (paused || tasks.empty());
                else
                    return (tasks_running == 0) && tasks.empty();
            });
"""
CREATE = """\
        {
            const std::scoped_lock tasks_lock(tasks_mutex);
            tasks_running = thread_count;
#ifndef __cpp_lib_jthread
            workers_running = true;
#endif
        }
"""
DESTROY = """\
        {
            const std::scoped_lock tasks_lock(tasks_mutex);
            workers_running = false;
        }
        task_available_cv.notify_all();
"""
WORKER_WAIT = """\
            task_available_cv.wait(tasks_lock BS_THREAD_POOL_WAIT_TOKEN,
"""
PORT_WORKER = """\
    void worker(BS_THREAD_POOL_WORKER_TOKEN const std::size_t idx)
    {
        this_thread::my_pool = this;
        this_thread::my_index = idx;
        init_func(idx);
        while (true)
        {
            // WTC port: the initialisation, or the last task, is done
            wtc_port::finished(wtc_counts, tasks_mutex, tasks_done_cv);
            task_t task;
            // WTC port: spin, then yield, until a task or the stop
            if (!wtc_port::pop(tasks, wtc_counts, task))
                break;
            wtc_busy[idx].v.store(true, std::memory_order_relaxed);  // WTC port
#ifdef __cpp_exceptions
            try
            {
#endif
                task();
#ifdef __cpp_exceptions
            }
            catch (...)
            {
            }
#endif
            wtc_busy[idx].v.store(false, std::memory_order_relaxed);  // WTC port
        }
        cleanup_func(idx);
        this_thread::my_index = std::nullopt;
        this_thread::my_pool = std::nullopt;
    }
"""

if not spin:
    patch(INCLUDE_AT, INCLUDE_AT + "\n#include \"wtc_q.h\"  // WTC port\n")
    patch("    static constexpr bool wait_deadlock_checks_enabled = (OptFlags & tp::wait_deadlock_checks) != tp::none;\n",
          "    static constexpr bool wait_deadlock_checks_enabled = (OptFlags & tp::wait_deadlock_checks) != tp::none;\n\n"
          "    static_assert(!priority_enabled && !pause_enabled,\n"
          "        \"the WTC port covers the pool without priorities and pausing\");  // WTC port\n")
    patch(DETACH_TASK,
          "        (void)priority;  // WTC port: the port has no priorities\n"
          "        wtc_port::push(tasks, wtc_counts, task_t(std::forward<F>(task)));  // WTC port: no mutex, no notification\n")
    patch(DETACH_BULK,
          "        (void)priority;  // WTC port: the port has no priorities\n"
          "        for (I it = first; it != last; ++it)  // WTC port: no mutex, no notification\n"
          "            wtc_port::push(tasks, wtc_counts, task_t(std::move(*it)));\n")
    patch("        const std::scoped_lock tasks_lock(tasks_mutex);\n        return tasks.size();\n",
          "        return wtc_port::queued(wtc_counts, wtc_busy.get(), thread_count);  // WTC port: a snapshot\n")
    patch("        const std::scoped_lock tasks_lock(tasks_mutex);\n        return tasks_running;\n",
          "        return wtc_port::running(wtc_busy.get(), thread_count);  // WTC port: a snapshot\n")
    patch("        const std::scoped_lock tasks_lock(tasks_mutex);\n        return tasks_running + tasks.size();\n",
          "        return wtc_counts.unfinished.load();  // WTC port: a snapshot\n")
    patch("        const std::scoped_lock tasks_lock(tasks_mutex);\n        tasks = {};\n",
          "        wtc_port::purge(tasks, wtc_counts, tasks_mutex, tasks_done_cv);  // WTC port\n")
    patch("        waiting = true;\n", "        wtc_counts.waiting.fetch_add(1);  // WTC port\n", 3)
    patch("        waiting = false;\n", "        wtc_counts.waiting.fetch_sub(1);  // WTC port\n", 3)
    patch(WAIT_PREDICATE, "            [this] { return wtc_port::idle(wtc_counts); });  // WTC port\n", 3)
    patch(CREATE,
          "        wtc_busy = std::make_unique<wtc_port::busy_flag[]>(thread_count);  // WTC port\n"
          "        wtc_counts.unfinished.fetch_add(thread_count);  // WTC port: until each worker is initialised\n"
          "        wtc_counts.stop.store(false);  // WTC port\n")
    patch(DESTROY, "        wtc_counts.stop.store(true, std::memory_order_release);  // WTC port\n")
    cut("    /**\n     * @brief Pop a task from the queue.\n", "        return task;\n    }\n\n", "")
    cut("    void worker(BS_THREAD_POOL_WORKER_TOKEN const std::size_t idx)\n",
        "        this_thread::my_pool = std::nullopt;\n    }\n", PORT_WORKER)
    patch("    std::conditional_t<priority_enabled, std::priority_queue<pr_task>, std::queue<task_t>> tasks;\n",
          "    wtc::queue<task_t> tasks{BS_WTC_TAG};  // WTC port: the compiler rewrites the tag\n\n"
          "    wtc_port::counts wtc_counts;  // WTC port: tasks unfinished, threads waiting, the stop\n\n"
          "    std::unique_ptr<wtc_port::busy_flag[]> wtc_busy;  // WTC port: which workers run a task\n")
    patch("    std::size_t tasks_running = 0;\n", "    // WTC port: in wtc_counts and wtc_busy\n")
    patch("    bool waiting = false;\n", "    // WTC port: in wtc_counts\n")
    patch("    bool workers_running = false;\n", "    // WTC port: in wtc_counts\n")
    s = ("// Altered by the WTC port (benchmarks/apps/bs-thread-pool/port.py): the\n"
         "// task queue is wtc::queue; every changed line is marked \"WTC port\".\n") + s
else:
    patch(INCLUDE_AT, INCLUDE_AT + "\n#define BS_WTC_CONTROL  // WTC control: wtc_q.h without the shim\n"
          "#include \"wtc_q.h\"  // WTC control\n")
    publish = "tasks.size() + (workers_running ? 0 : 1)"
    patch(WORKER_WAIT,
          "            wtc_port::spin_wait(tasks_lock, wtc_ready,  // WTC control\n")
    patch("            else\n                tasks.emplace(std::forward<F>(task));\n        }\n",
          "            else\n                tasks.emplace(std::forward<F>(task));\n"
          f"            wtc_ready.store({publish}, std::memory_order_release);  // WTC control\n        }}\n")
    patch("                        tasks.emplace(std::move(*it));\n                }\n            }\n",
          "                        tasks.emplace(std::move(*it));\n                }\n"
          f"                wtc_ready.store({publish}, std::memory_order_release);  // WTC control\n            }}\n")
    patch("        tasks = {};\n",
          f"        tasks = {{}};\n        wtc_ready.store({publish}, std::memory_order_release);  // WTC control\n")
    patch("            task = std::move(tasks.front());\n        tasks.pop();\n",
          "            task = std::move(tasks.front());\n        tasks.pop();\n"
          f"        wtc_ready.store({publish}, std::memory_order_release);  // WTC control\n")
    patch("            workers_running = false;\n",
          "            workers_running = false;\n"
          f"            wtc_ready.store({publish}, std::memory_order_release);  // WTC control\n")
    patch("    bool workers_running = false;\n",
          "    bool workers_running = false;\n\n"
          "    std::atomic<std::size_t> wtc_ready{0};  // WTC control: tasks queued, plus one once stopping\n")
    s = ("// Altered by the WTC port's control (benchmarks/apps/bs-thread-pool/port.py\n"
         "// --orig-spin): workers spin on a published count instead of sleeping;\n"
         "// every changed line is marked \"WTC control\".\n") + s
shutil.copy(os.path.join(here, "wtc_q.h"), os.path.join(out, "wtc_q.h"))
open(os.path.join(out, NAME), "w").write(s)
print(f"port: {out}/{NAME}{' (control)' if spin else ''}")
