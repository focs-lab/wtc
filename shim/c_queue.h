/* The queue the compiler puts in place of a queue a C program wrote itself:
   a ring of pointers under a pthread mutex, with condition variables to
   wait on, and a count of the producers that have finished
   (--wtc-lift-c-queues, docs/lifting.md "C queues"). The program never
   includes this header: the pass replaces the bodies of the program's queue
   functions with calls to the functions below, through functions of its
   own per struct that take a pointer to the struct and carry the
   annotations the lifting pass raises to the dialect's queue operations
   (wtc_queue_push_wait, wtc_queue_pop_wait, wtc_queue_push_batch,
   wtc_queue_pop_batch, wtc_queue_construction), so that a queue's type is
   the program's struct. tools/wtc-cc links their definitions (c_queue.cpp).
   Nothing else in the program changes.

   The program's queue object keeps its layout. Its first eight bytes hold
   the pointer to the runtime's queue, built by wtc_c_queue_init and freed
   by wtc_c_queue_destroy; the pass proves first that nothing else in the
   program touches the object.

   Every queue is bounded with the ring's own capacity, exactly
   (runtime/bounded_ring.hpp), and its elements are the program's
   pointers. A push waits while the queue is full; a pop waits while it is
   empty and not finished, and answers "finished" once it is empty and as
   many producers as the program said at its initialisation have called
   wtc_c_queue_terminate: the counted close, which is the original's
   `end_count == prod_threads`. A pop that waits keeps the original's sleep:
   a short spin, then sleep, never spinning without end.

   The batched forms move elements between the queue and a ring the
   calling thread owns (dedup's ringbuffer_t on its stack), described by
   where its data pointer, its two indices and its slot count are: the
   index at which the program inserts (`in`), the one at which it removes
   (`out`), the ring full when (in + 1) % slots == out and empty when in ==
   out. A batch is taken or appended at one step, one compare-and-swap of
   the word that holds both of the ring's counters, which decides its size
   and claims its tickets: its elements are adjacent in the queue, whatever
   other threads do on either side, and its size is what the queue allowed
   at that step, as under the original's mutex. */
#ifndef WTC_C_QUEUE_H
#define WTC_C_QUEUE_H

#ifdef __cplusplus
extern "C" {
#endif

/* The construction, whose tag the specialisation pass rewrites: tag is the
   topology (shim/topology.hpp), capacity the ring's slots minus one,
   producers the number of terminations that finish the queue, and ends how
   it finishes, as the program's tests of its end count read: 0 never (a
   queue the program never finishes; producers is ignored), 1 when exactly
   producers have terminated (`end_count == producers`), 2 when at least
   that many have (`end_count >= producers`, `end_count < producers`
   only). */
void wtc_c_queue_init(void *at, int tag, long capacity, int producers, int ends);

/* Waits until no operation is still under way in the runtime, then frees
   the queue: a thread that has seen an operation's effect may destroy the
   queue at once, as it may the original once that operation has released
   the mutex. */
void wtc_c_queue_destroy(void *at);

/* Waits while the queue is full, then appends v. */
void wtc_c_queue_push_wait(void *at, void *v);

/* Waits for an element and stores it to *out: 1; or 0 once the queue is
   finished and empty. */
int wtc_c_queue_pop_wait(void *at, void **out);

/* One producer has finished; the last of them finishes the queue. More
   terminations than producers change nothing when the queue finishes at
   "at least" (ends 2), as in the original; they end the program when it
   finishes at "exactly" (ends 1), where the original goes wrong (ferret's
   pop takes from an empty ring then). */
void wtc_c_queue_terminate(void *at);

/* Waits while the queue is full, then appends at one step as many of the
   caller's ring's elements, oldest first, as fit, up to limit; returns how
   many. With none to move, it only waits while the queue is full. The limit
   is a count: the program's, made no less than zero. */
int wtc_c_queue_push_batch(void *at, void ***data, int *in, int *out, unsigned long slots, unsigned long limit);

/* Waits while the queue is empty and not finished; -1 once it is finished
   and empty; otherwise takes at one step as many of its oldest elements as
   the caller's ring has room for, up to limit, and returns how many (0
   when the ring is full or limit is zero). With several consumers, their
   batches are disjoint runs of the queue. */
int wtc_c_queue_pop_batch(void *at, void ***data, int *in, int *out, unsigned long slots, unsigned long limit);

#ifdef __cplusplus
}
#endif

#endif
