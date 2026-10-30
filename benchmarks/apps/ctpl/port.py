#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Derived from CTPL (fetched by fetch.sh beside this script), Apache
# License 2.0; see LICENSE.
"""Makes the hand port's ctpl_stl.h out of the library's: a copy in which
the queue class detail::Queue keeps its interface and holds a wtc::queue
instead of a std::queue under a mutex, and nothing else changes.

    port.py <ctpl-src> <out-dir>

What the port changes, exactly (every changed line is marked "WTC port"):
  - ctpl_stl.h includes the shim's queue.hpp;
  - detail::Queue holds a wtc::queue<T>, built with CTPL_WTC_TAG, which is
    empty (the general-purpose tag, for the compiler to choose) unless a
    build defines it; push pushes, pop is the shim's try_pop, empty goes
    (nothing in the pool calls it), and the mutex goes.

The pool is the library's: it still waits on its own condition variable
under its own mutex, with the queue's pop as the predicate, and a push
still takes that mutex before it notifies, which is what keeps a worker's
check and its sleep atomic with respect to a push. That is the form the
lifting pass gives the queue too (docs/lifting.md), so the port and the
lifted program differ in how the queue class is written, not in what the
pool does with it.
"""
import os
import shutil
import sys

src, out = sys.argv[1], sys.argv[2]
if os.path.exists(out):
    shutil.rmtree(out)
os.makedirs(out)
s = open(os.path.join(src, "ctpl_stl.h")).read()

old_queue = s[s.index("        template <typename T>\n        class Queue {"):s.index("    class thread_pool {")]
new_queue = """        template <typename T>
        class Queue {  // WTC port
        public:  // WTC port
#ifndef CTPL_WTC_TAG  // WTC port
#define CTPL_WTC_TAG wtc::topology::generic  // WTC port
#endif  // WTC port
            Queue() : q(CTPL_WTC_TAG) {}  // WTC port
            bool push(T const & value) {  // WTC port
                this->q.push(value);  // WTC port
                return true;  // WTC port
            }  // WTC port
            bool pop(T & v) { return this->q.try_pop(v); }  // WTC port
        private:  // WTC port
            wtc::queue<T> q;  // WTC port
        };  // WTC port
    }

"""
assert old_queue.count("class Queue {") == 1
s = s.replace(old_queue, new_queue)
old = "#include <queue>\n"
assert s.count(old) == 1
s = s.replace(old, old + "#include \"queue.hpp\"  // WTC port\n")
s = "// Altered from CTPL's ctpl_stl.h by benchmarks/apps/ctpl/port.py (lines marked \"WTC port\").\n" + s
open(os.path.join(out, "ctpl_stl.h"), "w").write(s)
