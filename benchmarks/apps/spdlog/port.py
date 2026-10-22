#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Derived from spdlog (https://github.com/gabime/spdlog), MIT; see LICENSE.
"""Makes the port's include tree out of spdlog's: a copy of include/ in
which the thread pool's queue is wtc::queue instead of the mutex-and-
condition-variable queue, and nothing else changes.

What changes, exactly:
  - spdlog/details/thread_pool.h includes wtc_q.h instead of
    mpmc_blocking_q.h; the pool's queue member is a wtc::queue<async_msg>
    and a wtc_bound beside it keeps the original queue's bound and
    counters;
  - spdlog/details/thread_pool-inl.h builds the bound where it built the
    queue, and calls the queue through the functions of wtc_q.h where it
    called the queue's methods;
  - spdlog/details/wtc_q.h (wtc_q.h beside this script) is added.

The pool, the async logger, the registry and the sinks are spdlog's own:
the workers are still started in the pool's constructor, in a loop over
threads_n, and the logging threads still reach the pool through the
logger's weak_ptr. That is what the analysis has to see through. The
queue is a member of the pool rather than of a queue class of its own so
that the one constructor builds the queue and starts the workers: that is
what lets the compiler choose the tag when the pool is built.

    port.py <spdlog-src> <out-include-dir>
"""
import os
import shutil
import sys

src, out = sys.argv[1], sys.argv[2]
here = os.path.dirname(os.path.abspath(__file__))

if os.path.exists(out):
    shutil.rmtree(out)
shutil.copytree(os.path.join(src, "include"), out)

def patch(path, pairs):
    s = open(path).read()
    for old, new in pairs:
        n = s.count(old)
        assert n == 1, f"{path}: expected one of {old!r}, found {n}"
        s = s.replace(old, new)
    open(path, "w").write(s)


patch(os.path.join(out, "spdlog", "details", "thread_pool.h"), [
    ("#include <spdlog/details/mpmc_blocking_q.h>\n",
     "#include <spdlog/details/wtc_q.h>  // WTC port: the queue\n"),
    ("    using q_type = details::mpmc_blocking_queue<item_type>;\n",
     "    using q_type = wtc::queue<item_type>;  // WTC port\n"),
    ("    q_type q_;\n",
     "    q_type q_{SPDLOG_WTC_TAG};  // WTC port: the compiler rewrites the tag\n"
     "    details::wtc_bound bound_;\n"),
])
patch(os.path.join(out, "spdlog", "details", "thread_pool-inl.h"), [
    ("    : q_(q_max_items) {\n", "    : bound_(q_max_items) {  // WTC port\n"),
    ("        q_.enqueue(std::move(new_msg));\n",
     "        wtc_enqueue(q_, bound_, std::move(new_msg));\n"),
    ("        q_.enqueue_nowait(std::move(new_msg));\n",
     "        wtc_enqueue_nowait(q_, bound_, std::move(new_msg));\n"),
    ("        q_.enqueue_if_have_room(std::move(new_msg));\n",
     "        wtc_enqueue_if_have_room(q_, bound_, std::move(new_msg));\n"),
    ("    q_.dequeue(incoming_async_msg);\n",
     "    wtc_dequeue(q_, bound_, incoming_async_msg);\n"),
    ("thread_pool::overrun_counter() { return q_.overrun_counter(); }",
     "thread_pool::overrun_counter() { return bound_.overrun.load(); }"),
    ("thread_pool::reset_overrun_counter() { q_.reset_overrun_counter(); }",
     "thread_pool::reset_overrun_counter() { bound_.overrun.store(0); }"),
    ("thread_pool::discard_counter() { return q_.discard_counter(); }",
     "thread_pool::discard_counter() { return bound_.discard.load(); }"),
    ("thread_pool::reset_discard_counter() { q_.reset_discard_counter(); }",
     "thread_pool::reset_discard_counter() { bound_.discard.store(0); }"),
    ("thread_pool::queue_size() { return q_.size(); }",
     "thread_pool::queue_size() { return bound_.size(); }"),
])
shutil.copy(os.path.join(here, "wtc_q.h"), os.path.join(out, "spdlog", "details", "wtc_q.h"))
print(f"port: {out}")
