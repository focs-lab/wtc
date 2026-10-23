#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Derived from PARSEC dedup (pkgs/kernels/dedup), Copyright (c) 2006-2012,
# Princeton University; see LICENSE here.
"""Turns PARSEC dedup's encoder (encoder.c, at the pinned corpus commit)
into a C++ program on wtc::queue: the one translation unit of dedup that
uses queues and starts threads.

dedup is the evaluation's case of a program the analysis must refuse as it
is written. Its four stage queues are arrays on the heap, one per group of
four threads, and a thread finds its queue by an index computed at run
time; the reorder queue has two producing stages, so it is not
single-producer at any thread count. What changes, and nothing else:

  - queue_t becomes wtc::queue<void*>: the arrays are still malloc'd, and
    each element is constructed where the original called queue_init;
  - queue_enqueue -> push each element of the sending ring buffer, up to
    the same limit;
  - queue_dequeue -> wait (spinning, pausing) for one element, then take
    what else is there up to the limit without waiting, as the original
    does under its lock;
  - queue_terminate -> the last producer of a queue pushes one null per
    consumer; a consumer that takes a null remembers it and its next take
    returns -1, the original's "empty and ended";
  - pthread_create -> wtc::spawn, a stage's statistics returned through
    its arguments, since a spawned routine returns nothing;
  - C to C++: the C headers in extern "C", casts from void*.

Three macros make the builds the evaluation compares, none of them the
program as written:
  DEDUP_ONE       one spawn per stage, no loop (requires -t 1);
  DEDUP_HAND_TAG  the tags an expert who knew the topology at -t 1 would
                  write: refine, deduplicate and compress single-producer
                  (spsc_inline or spsc, the macro's value), reorder general
                  (requires -t 1);
  DEDUP_FLAT      the four queues as globals rather than arrays, valid
                  while one group of queues serves every thread (-t 4 or
                  fewer): what the analysis sees once arrays of queues
                  are no obstacle;
  DEDUP_UNITY     dedup's main (dedup.c, the optional third argument) in
                  this translation unit: the threads start in Encode,
                  which main calls once, and only from here can the
                  analysis see that; built without dedup.c's object.

    port.py <encoder.c> <out.cpp> [<dedup.c>]
"""
import sys

src, out = sys.argv[1], sys.argv[2]
s = open(src).read()


def rep(old, new, count=1):
    global s
    n = s.count(old)
    assert n == count, f"expected {count} of {old[:60]!r}, found {n}"
    s = s.replace(old, new)


# Headers: dedup's own and the C library's, in extern "C" so the program's
# other files, still built as C, link with this one.
rep('''#include <assert.h>
#include <strings.h>
#include <math.h>
#include <limits.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
''', '''#include <atomic>
#include <new>
#include <thread>
#include <vector>
extern "C" {
#include <assert.h>
#include <strings.h>
#include <math.h>
#include <limits.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
''')
hooks = "#ifdef ENABLE_PARSEC_HOOKS\n#include <hooks.h>\n#endif //ENABLE_PARSEC_HOOKS"
assert s.count(hooks) == 1
hooks_end = s.index(hooks) + len(hooks)
s = s[:hooks_end] + '''
#ifdef DEDUP_UNITY
#include "decoder.h"
#endif
}
#include "queue.hpp"
#include "thread.hpp"''' + s[hooks_end:]

# The queues and their ends.
rep("queue_t *deduplicate_que, *refine_que, *reorder_que, *compress_que;", '''/* WTC port: the stage queues carry chunk pointers. A queue ends when its
   last producer pushes one null per consumer (wq_end below). */
typedef wtc::queue<void *> wq_t;
struct wq_count { std::atomic<int> left; int consumers; };

#ifdef DEDUP_HAND_TAG
#define DEDUP_SPSC wtc::topology::DEDUP_HAND_TAG
#else
#define DEDUP_SPSC wtc::topology::generic
#endif

#ifdef DEDUP_FLAT
static wq_t deduplicate_que{DEDUP_SPSC}, refine_que{DEDUP_SPSC}, reorder_que, compress_que{DEDUP_SPSC};
static wq_count deduplicate_que_end, refine_que_end, reorder_que_end, compress_que_end;
#define QUE(q, i) (&(q))
#define END(q, i) (&q##_end)
#else
wq_t *deduplicate_que, *refine_que, *reorder_que, *compress_que;
static wq_count *deduplicate_que_end, *refine_que_end, *reorder_que_end, *compress_que_end;
#define QUE(q, i) (&(q)[i])
#define END(q, i) (&q##_end[i])
#endif

/* queue_enqueue: up to limit elements of the sending buffer, in order. */
static int wq_put(wq_t *q, ringbuffer_t *buf, int limit) {
  int i;
  for (i = 0; i < limit && !ringbuffer_isEmpty(buf); i++) q->push(ringbuffer_remove(buf));
  return i;
}

/* queue_dequeue: wait for one element, then take what else is there up to
   the limit. -1 once this consumer has taken the queue's end. */
static int wq_take(wq_t *q, ringbuffer_t *buf, int limit, int *ended) {
  if (*ended) return -1;
  void *p;
  for (;;) {
    if (auto e = q->try_pop()) { p = *e; break; }
    __builtin_ia32_pause();
  }
  if (p == NULL) { *ended = 1; return -1; }
  int r = ringbuffer_insert(buf, p);
  assert(r == 0);
  int i = 1;
  while (i < limit && !ringbuffer_isFull(buf)) {
    auto e = q->try_pop();
    if (!e) break;
    if (*e == NULL) { *ended = 1; break; }
    r = ringbuffer_insert(buf, *e);
    assert(r == 0);
    i++;
  }
  return i;
}

/* queue_terminate: the last producer ends the queue for every consumer. */
static void wq_end(wq_t *q, wq_count *c) {
  if (c->left.fetch_sub(1) == 1)
    for (int i = 0; i < c->consumers; ++i) q->push(NULL);
}

static void wq_count_init(wq_count *c, int producers, int consumers) {
  c->left = producers;
  c->consumers = consumers;
}''')

# A stage's statistics come back through its arguments.
rep('''  struct {
    void *buffer;
    size_t size;
  } input_file;
};''', '''  struct {
    void *buffer;
    size_t size;
  } input_file;
  //WTC port: what the stage returned
  void *rv;
};''')

# C to C++: pointers out of void*.
for decl in ("stats_t *thread_stats = malloc(sizeof(stats_t));",):
    rep(decl, decl.replace("= malloc", "= (stats_t *)malloc"), count=3)
rep("u32int * rabintab = malloc(256*sizeof rabintab[0]);",
    "u32int * rabintab = (u32int *)malloc(256*sizeof rabintab[0]);", count=3)
rep("u32int * rabinwintab = malloc(256*sizeof rabintab[0]);",
    "u32int * rabinwintab = (u32int *)malloc(256*sizeof rabintab[0]);", count=3)
rep("chunks_per_anchor = malloc(", "chunks_per_anchor = (sequence_number_t *)malloc(")
rep("chunks_per_anchor = realloc(", "chunks_per_anchor = (sequence_number_t *)realloc(")
rep("write_file(fd, TYPE_COMPRESS, chunk->compressed_data.n, chunk->compressed_data.ptr);",
    "write_file(fd, TYPE_COMPRESS, chunk->compressed_data.n, (u_char *)chunk->compressed_data.ptr);", count=2)
rep("r = compress(chunk->compressed_data.ptr, &n, chunk->uncompressed_data.ptr, chunk->uncompressed_data.n);",
    "r = compress((Bytef *)chunk->compressed_data.ptr, &n, (const Bytef *)chunk->uncompressed_data.ptr, chunk->uncompressed_data.n);")
rep("rabinseg(chunk->uncompressed_data.ptr,", "rabinseg((uchar *)chunk->uncompressed_data.ptr,", count=2)
rep("rabinseg(chunk->uncompressed_data.ptr + ANCHOR_JUMP,", "rabinseg((uchar *)chunk->uncompressed_data.ptr + ANCHOR_JUMP,")
rep("memcpy(chunk->uncompressed_data.ptr+bytes_left, args->input_file.buffer+preloading_buffer_seek, max_read);",
    "memcpy((char *)chunk->uncompressed_data.ptr+bytes_left, (char *)args->input_file.buffer+preloading_buffer_seek, max_read);",
    count=2)
rep("r = read(fd, chunk->uncompressed_data.ptr+bytes_left+bytes_read, MAXBUF-bytes_read);",
    "r = read(fd, (char *)chunk->uncompressed_data.ptr+bytes_left+bytes_read, MAXBUF-bytes_read);", count=2)
rep("r = read(fd, preloading_buffer+bytes_read, filestat.st_size-bytes_read);",
    "r = read(fd, (char *)preloading_buffer+bytes_read, filestat.st_size-bytes_read);")

# Producers.
for q in ("reorder_que", "compress_que", "deduplicate_que", "refine_que"):
    s = s.replace(f"queue_enqueue(&{q}[qid],", f"wq_put(QUE({q}, qid),")
assert "queue_enqueue(&" not in s, "a queue_enqueue left unported"

# Consumers: each stage thread takes from one queue, and remembers its end.
ENDED = "  int ended = 0; //WTC port: this thread has taken its queue's end\n"
for q, limit, bufdecl in (("compress_que", "ITEM_PER_FETCH", "  ringbuffer_t recv_buf, send_buf;\n\n"),
                          ("deduplicate_que", "CHUNK_ANCHOR_PER_FETCH", "  ringbuffer_t recv_buf, send_buf_reorder, send_buf_compress;\n"),
                          ("refine_que", "MAX_PER_FETCH", "  ringbuffer_t recv_buf, send_buf;\n  int r;\n")):
    rep(f"r = queue_dequeue(&{q}[qid], &recv_buf, {limit});", f"r = wq_take(QUE({q}, qid), &recv_buf, {limit}, &ended);")
    rep(bufdecl, ENDED + bufdecl)
# Reorder takes from every reorder queue in turn.
rep("r = queue_dequeue(&reorder_que[qid], &recv_buf, ITEM_PER_FETCH);",
    "r = wq_take(QUE(reorder_que, qid), &recv_buf, ITEM_PER_FETCH, &ended[qid]);")
rep('''  r = ringbuffer_init(&recv_buf, ITEM_PER_FETCH);
  assert(r==0);

  fd = create_output_file(conf->outfile);''', '''  r = ringbuffer_init(&recv_buf, ITEM_PER_FETCH);
  assert(r==0);
  std::vector<int> ended(args->nqueues); //WTC port: which queues have ended

  fd = create_output_file(conf->outfile);''')
assert "queue_dequeue(&" not in s, "a queue_dequeue left unported"

# Ends.
for q in ("reorder_que", "compress_que", "deduplicate_que"):
    rep(f"queue_terminate(&{q}[qid]);", f"wq_end(QUE({q}, qid), END({q}, qid));")
rep("queue_terminate(&refine_que[i]);", "wq_end(QUE(refine_que, i), END(refine_que, i));")

# Setup: where the original allocates and initialises the queues.
for q in ("deduplicate_que", "refine_que", "reorder_que", "compress_que"):
    rep(f"  {q} = malloc(sizeof(queue_t) * nqueues);\n",
        f"#ifndef DEDUP_FLAT\n  {q} = (wq_t *)malloc(sizeof(wq_t) * nqueues);\n"
        f"  {q}_end = new wq_count[nqueues];\n#endif\n")
rep("  if( (deduplicate_que == NULL) || (refine_que == NULL) || (reorder_que == NULL) || (compress_que == NULL)) {",
    "#ifdef DEDUP_FLAT\n  if (nqueues != 1) EXIT_TRACE(\"this build has one group of queues: -t %d is more than %d\\n\", conf->nthreads, MAX_THREADS_PER_QUEUE);\n#else\n"
    "  if( (deduplicate_que == NULL) || (refine_que == NULL) || (reorder_que == NULL) || (compress_que == NULL)) {")
rep('''    printf("Out of memory\\n");
    exit(1);
  }
  int threads_per_queue;''', '''    printf("Out of memory\\n");
    exit(1);
  }
#endif
#if defined(DEDUP_HAND_TAG) || defined(DEDUP_ONE)
  if (conf->nthreads != 1) EXIT_TRACE("this build runs one thread per stage: -t 1\\n");
#endif
  int threads_per_queue;''')
rep('''    queue_init(&deduplicate_que[i], QUEUE_SIZE, threads_per_queue);
    queue_init(&refine_que[i], QUEUE_SIZE, 1);
    queue_init(&reorder_que[i], QUEUE_SIZE, threads_per_queue);
    queue_init(&compress_que[i], QUEUE_SIZE, threads_per_queue);''', '''    //WTC port: the queues are unbounded, and each records who ends it and
    //how many consumers it ends for. The stage that fills reorder_que
    //besides compress (deduplicate) has finished before compress ends.
#ifndef DEDUP_FLAT
    new (&deduplicate_que[i]) wq_t(DEDUP_SPSC);
    new (&refine_que[i]) wq_t(DEDUP_SPSC);
    new (&reorder_que[i]) wq_t;
    new (&compress_que[i]) wq_t(DEDUP_SPSC);
#endif
    wq_count_init(END(deduplicate_que, i), threads_per_queue, threads_per_queue);
    wq_count_init(END(refine_que, i), 1, threads_per_queue);
    wq_count_init(END(reorder_que, i), threads_per_queue, 1);
    wq_count_init(END(compress_que, i), threads_per_queue, threads_per_queue);''')

# The threads.
start = s.index("  pthread_t threads_anchor[MAX_THREADS],")
end = s.index("#ifdef ENABLE_PARSEC_HOOKS\n  __parsec_roi_end();\n#endif\n\n  /* free queues */")
s = s[:start] + '''  data_process_args.tid = 0;
  data_process_args.nqueues = nqueues;
  data_process_args.fd = fd;

#ifdef ENABLE_PARSEC_HOOKS
    __parsec_roi_begin();
#endif

  std::vector<struct thread_args> anchor_thread_args(conf->nthreads);
  std::vector<struct thread_args> chunk_thread_args(conf->nthreads);
  std::vector<struct thread_args> compress_thread_args(conf->nthreads);
  struct thread_args send_block_args;
  send_block_args.tid = 0;
  send_block_args.nqueues = nqueues;
  for (i = 0; i < conf->nthreads; i ++)
    anchor_thread_args[i].tid = chunk_thread_args[i].tid = compress_thread_args[i].tid = i;

#ifdef DEDUP_ONE
  std::thread threads_process = wtc::spawn(Fragment_t, &data_process_args);
  std::thread threads_anchor = wtc::spawn(FragmentRefine_t, &anchor_thread_args[0]);
  std::thread threads_chunk = wtc::spawn(Deduplicate_t, &chunk_thread_args[0]);
  std::thread threads_compress = wtc::spawn(Compress_t, &compress_thread_args[0]);
  std::thread threads_send = wtc::spawn(Reorder_t, &send_block_args);
  threads_process.join();
  threads_anchor.join();
  threads_chunk.join();
  threads_compress.join();
  threads_send.join();
#else
  //thread for first pipeline stage (input)
  std::thread threads_process = wtc::spawn(Fragment_t, &data_process_args);

  //Create 3 thread pools for the intermediate pipeline stages
  std::vector<std::thread> threads_anchor, threads_chunk, threads_compress;
  for (i = 0; i < conf->nthreads; i ++)
    threads_anchor.push_back(wtc::spawn(FragmentRefine_t, &anchor_thread_args[i]));
  for (i = 0; i < conf->nthreads; i ++)
    threads_chunk.push_back(wtc::spawn(Deduplicate_t, &chunk_thread_args[i]));
  for (i = 0; i < conf->nthreads; i ++)
    threads_compress.push_back(wtc::spawn(Compress_t, &compress_thread_args[i]));

  //thread for last pipeline stage (output)
  std::thread threads_send = wtc::spawn(Reorder_t, &send_block_args);

  /*** parallel phase ***/

  //join all threads
  threads_process.join();
  for (i = 0; i < conf->nthreads; i ++) threads_anchor[i].join();
  for (i = 0; i < conf->nthreads; i ++) threads_chunk[i].join();
  for (i = 0; i < conf->nthreads; i ++) threads_compress[i].join();
  threads_send.join();
#endif

  //Return values of threads
  std::vector<stats_t *> threads_anchor_rv(conf->nthreads);
  std::vector<stats_t *> threads_chunk_rv(conf->nthreads);
  std::vector<stats_t *> threads_compress_rv(conf->nthreads);
  for (i = 0; i < conf->nthreads; i ++) {
    threads_anchor_rv[i] = (stats_t *)anchor_thread_args[i].rv;
    threads_chunk_rv[i] = (stats_t *)chunk_thread_args[i].rv;
    threads_compress_rv[i] = (stats_t *)compress_thread_args[i].rv;
  }

''' + s[end:]
rep('''  for(i=0; i<nqueues; i++) {
    queue_destroy(&deduplicate_que[i]);
    queue_destroy(&refine_que[i]);
    queue_destroy(&reorder_que[i]);
    queue_destroy(&compress_que[i]);
  }
  free(deduplicate_que);
  free(refine_que);
  free(reorder_que);
  free(compress_que);''', '''#ifndef DEDUP_FLAT
  for(i=0; i<nqueues; i++) {
    deduplicate_que[i].~wq_t();
    refine_que[i].~wq_t();
    reorder_que[i].~wq_t();
    compress_que[i].~wq_t();
  }
  free(deduplicate_que);
  free(refine_que);
  free(reorder_que);
  free(compress_que);
  delete[] deduplicate_que_end;
  delete[] refine_que_end;
  delete[] reorder_que_end;
  delete[] compress_que_end;
#endif''')

# Spawned routines return nothing; the stages' results go to their arguments.
encode = s.index("void Encode(config_t * _conf)")
s = s[:encode] + '''#ifdef ENABLE_PTHREADS
//WTC port: the routines wtc::spawn starts
static void Fragment_t(void *a) { ((struct thread_args *)a)->rv = Fragment(a); }
static void FragmentRefine_t(void *a) { ((struct thread_args *)a)->rv = FragmentRefine(a); }
static void Deduplicate_t(void *a) { ((struct thread_args *)a)->rv = Deduplicate(a); }
static void Compress_t(void *a) { ((struct thread_args *)a)->rv = Compress(a); }
static void Reorder_t(void *a) { ((struct thread_args *)a)->rv = Reorder(a); }
#endif

''' + s[encode:]

# The whole program in one translation unit: dedup.c after its includes and
# its definition of conf, which in C is one object with encoder.c's (both
# are tentative definitions) and in C++ would be a second.
if len(sys.argv) > 3:
    m = open(sys.argv[3]).read()
    conf = "config_t * conf;\n"
    assert m.count(conf) == 1
    s += ("\n#ifdef DEDUP_UNITY\n/* WTC port: dedup.c from here on */\n"
          + m[m.index(conf) + len(conf):] + "#endif //DEDUP_UNITY\n")

open(out, "w").write(s)
