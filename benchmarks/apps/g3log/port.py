#!/usr/bin/env python3
# SPDX-License-Identifier: Unlicense
# Derived from g3log (fetched by fetch.sh beside this script), in the
# public domain; see LICENSE.
"""Writes the hand port's shared_queue.hpp: g3log's queue class with its
interface kept, holding a wtc::queue instead of a std::queue under a mutex
and a condition variable. It goes into <out-dir>/g3log/, which the build
puts on the include path before g3log's own src/, so that active.hpp's
`#include "g3log/shared_queue.hpp"` finds it; nothing else changes.

    port.py <g3log-src> <out-dir>

The class (every line is the port's):
  - holds a wtc::queue<T>, built with G3LOG_WTC_TAG, the general-purpose
    tag unless a build defines it (the hand-declared build defines mpsc:
    every Active object's queue is popped by the one thread that object
    starts);
  - push moves its argument in; wait_and_pop is the shim's pop_wait,
    which spins briefly and then sleeps, as the original slept on its
    condition variable; try_and_pop is the shim's try_pop;
  - empty and size are not kept: g3log does not call them.
"""
import os
import shutil
import sys

src, out = sys.argv[1], sys.argv[2]
assert os.path.exists(os.path.join(src, "src", "g3log", "shared_queue.hpp"))
if os.path.exists(out):
    shutil.rmtree(out)
os.makedirs(os.path.join(out, "g3log"))
open(os.path.join(out, "g3log", "shared_queue.hpp"), "w").write('''\
// The hand port of g3log's shared_queue.hpp, written by
// benchmarks/apps/g3log/port.py: the class's interface over wtc::queue.
#pragma once

#include "queue.hpp"

#ifndef G3LOG_WTC_TAG
#define G3LOG_WTC_TAG wtc::topology::generic
#endif

template <typename T>
class shared_queue {
   wtc::queue<T> queue_{G3LOG_WTC_TAG, wtc::wait_policy::spin_then_park};

   shared_queue& operator=(const shared_queue&) = delete;
   shared_queue(const shared_queue& other) = delete;

  public:
   shared_queue() = default;

   void push(T item) { queue_.push(std::move(item)); }

   bool try_and_pop(T& popped_item) { return queue_.try_pop(popped_item); }

   void wait_and_pop(T& popped_item) { queue_.pop_wait(popped_item); }
};
''')
