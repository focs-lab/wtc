#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Derived from the Ferret Toolkit (PARSEC pkgs/apps/ferret), whose sources
# are under the GNU GPL, version 2 or any later version; see LICENSE here.
"""Turns PARSEC ferret's pthreads pipeline (ferret-pthreads.c, at the pinned
corpus commit) into a C++ program on wtc::queue, the one translation unit
of ferret that uses queues and starts threads.

What changes, and nothing else:
  - the five `struct queue` globals become `wtc::queue<T*>` of the element
    each carries (load_data*, seg_data*, ...);
  - enqueue -> push; a blocking dequeue -> a spin on try_pop, pausing,
    as every variant measured does (threads never outnumber cores);
  - queue_signal_terminate -> the last producer of a queue pushes one
    null element per consumer, which ends each consumer's loop;
  - the thread pool (tpool_create, in another translation unit) -> direct
    wtc::spawn calls in main, one thread per stage when FERRET_ONE is
    defined, n per middle stage (argv[6]) otherwise;
  - C to C++: the C headers in extern "C", stage functions void(void*).

    port.py <ferret-pthreads.c> <out.cpp>
"""
import sys

src, out = sys.argv[1], sys.argv[2]
s = open(src).read()


def rep(old, new, count=1):
    global s
    n = s.count(old)
    assert n == count, f"expected {count} of {old[:60]!r}, found {n}"
    s = s.replace(old, new)


rep('''#include <pthread.h>
#include <cass.h>
#include <cass_timer.h>
#include <../image/image.h>
#include "tpool.h"
#include "queue.h"
''', '''#include <pthread.h>
#include <atomic>
#include <thread>
#include <vector>
extern "C" {
#include <cass.h>
#include <cass_timer.h>
#include <../image/image.h>
}
#include "queue.hpp"
#include "thread.hpp"

/* WTC port: every stage queue carries pointers to the stage's record. The
   last producer of a queue ends it by pushing one null per consumer. */
template <class T> static T* take(wtc::queue<T*>& q) {
	for (;;) {
		if (auto p = q.try_pop()) return *p;
		__builtin_ia32_pause();
	}
}
template <class T> static void end_of(wtc::queue<T*>& q, std::atomic<int>& producers_left, int consumers) {
	if (producers_left.fetch_sub(1) == 1)
		for (int i = 0; i < consumers; ++i) q.push(nullptr);
}
''')
rep('char *extra_params = "-L 8 - T 20";', 'char *extra_params = (char *)"-L 8 - T 20";')
# FERRET_HAND_TAG is what an expert who knew the topology would write: every
# queue declared inline, for the one-thread-per-stage build.
rep("#define DEFAULT_DEPTH", """#ifdef FERRET_HAND_TAG
#define FERRET_TAG (wtc::topology::spsc_inline)
#else
#define FERRET_TAG
#endif

#define DEFAULT_DEPTH""")
for qname, tname in (("q_load_seg", "load_data"), ("q_seg_extract", "seg_data"),
                     ("q_extract_vec", "extract_data"), ("q_vec_rank", "vec_query_data"),
                     ("q_rank_out", "rank_data")):
    rep(f"struct queue {qname};", f"wtc::queue<struct {tname}*> {qname} FERRET_TAG;\nstd::atomic<int> {qname}_left;")
# producers
rep("enqueue(&q_load_seg, data);", "q_load_seg.push(data);")
for q in ("q_seg_extract", "q_extract_vec", "q_vec_rank", "q_rank_out"):
    var = {"q_seg_extract": "seg", "q_extract_vec": "extract", "q_vec_rank": "vec", "q_rank_out": "rank"}[q]
    rep(f"enqueue(&{q}, {var});", f"{q}.push({var});")
# terminations: the consumers of each queue
ends = {"q_load_seg": "NTHREAD_SEG", "q_seg_extract": "NTHREAD_EXTRACT", "q_extract_vec": "NTHREAD_VEC",
        "q_vec_rank": "NTHREAD_RANK", "q_rank_out": "NTHREAD_OUT"}
for q, cons in ends.items():
    rep(f"queue_signal_terminate(&{q});", f"end_of({q}, {q}_left, {cons});")
# consumers
for q, var in (("q_load_seg", "load"), ("q_seg_extract", "seg"), ("q_extract_vec", "extract"),
               ("q_vec_rank", "vec"), ("q_rank_out", "rank")):
    rep(f"if(dequeue(&{q}, &{var}) < 0)", f"if(({var} = take({q})) == NULL)")
# stage signatures
for st in ("t_load", "t_seg", "t_extract", "t_vec", "t_rank", "t_out"):
    rep(f"void *{st} (void *dummy)", f"void {st} (void *dummy)")
rep("	return NULL;\n", "	return;\n", count=6)
# main: queue setup and the threads
start = s.index("\tqueue_init(&q_load_seg,")
end = s.index("#ifdef ENABLE_PARSEC_HOOKS\n\t__parsec_roi_end();")
s = s[:start] + '''	q_load_seg_left = NTHREAD_LOAD;
	q_seg_extract_left = NTHREAD_SEG;
	q_extract_vec_left = NTHREAD_EXTRACT;
	q_vec_rank_left = NTHREAD_VEC;
	q_rank_out_left = NTHREAD_RANK;

	cnt_enqueue = cnt_dequeue = 0;

#ifdef ENABLE_PARSEC_HOOKS
	__parsec_roi_begin();
#endif
#ifdef FERRET_ONE
	/* one thread per stage, known when the program is compiled */
	std::thread th_load = wtc::spawn(t_load, (void *)query_dir);
	std::thread th_seg = wtc::spawn(t_seg, NULL);
	std::thread th_extract = wtc::spawn(t_extract, NULL);
	std::thread th_vec = wtc::spawn(t_vec, NULL);
	std::thread th_rank = wtc::spawn(t_rank, NULL);
	std::thread th_out = wtc::spawn(t_out, NULL);
	th_out.join(); th_rank.join(); th_vec.join(); th_extract.join(); th_seg.join(); th_load.join();
#else
	std::vector<std::thread> pool;
	pool.push_back(wtc::spawn(t_load, (void *)query_dir));
	for (i = 0; i < NTHREAD_SEG; i++) pool.push_back(wtc::spawn(t_seg, NULL));
	for (i = 0; i < NTHREAD_EXTRACT; i++) pool.push_back(wtc::spawn(t_extract, NULL));
	for (i = 0; i < NTHREAD_VEC; i++) pool.push_back(wtc::spawn(t_vec, NULL));
	for (i = 0; i < NTHREAD_RANK; i++) pool.push_back(wtc::spawn(t_rank, NULL));
	pool.push_back(wtc::spawn(t_out, NULL));
	for (auto &t : pool) t.join();
#endif

''' + s[end:]
# what main no longer needs
start = s.index("\ttpool_destroy(p_load);")
end = s.index("\tstimer_tuck(&tmr, \"QUERY TIME\");")
s = s[:start] + s[end:]
for decl in ("tdesc_t *t_load_desc;", "tdesc_t *t_seg_desc;", "tdesc_t *t_extract_desc;", "tdesc_t *t_vec_desc;",
             "tdesc_t *t_rank_desc;", "tdesc_t *t_out_desc;", "tpool_t *p_load;", "tpool_t *p_seg;",
             "tpool_t *p_extract;", "tpool_t *p_vec;", "tpool_t *p_rank;", "tpool_t *p_out;"):
    rep("\t" + decl + "\n", "")
rep("#ifdef FERRET_ONE", "#ifdef FERRET_ONE")  # sanity: exactly one thread-start block
# C to C++: the conversions C makes implicitly
rep("image_segment(&seg->mask,", "image_segment((void **)&seg->mask,")
rep("ret = cass_env_open(&env, db_dir, 0);", "ret = cass_env_open(&env, (char *)db_dir, 0);")
rep("table = query_table = cass_reg_get(&env->table, i);", "table = query_table = (cass_table_t *)cass_reg_get(&env->table, i);")
rep("\t\tquery_table = cass_reg_get(&env->table, i);", "\t\tquery_table = (cass_table_t *)cass_reg_get(&env->table, i);")
# the one-thread-per-stage build fixes the counts it was compiled for
rep("""	DEPTH = atoi(argv[5]);
	NTHREAD_SEG = atoi(argv[6]);""", """	DEPTH = atoi(argv[5]);
#ifdef FERRET_ONE
	if (atoi(argv[6]) != 1) { printf("this build runs one thread per stage; <n> must be 1\\n"); return 1; }
#endif
	NTHREAD_SEG = atoi(argv[6]);""")
open(out, "w").write(s)
