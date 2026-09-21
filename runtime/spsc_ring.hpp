// wtc::rt::spsc_ring: an unbounded single-producer single-consumer queue.
//
// The fact the compiler must prove before substituting this for wtc::queue:
//
//   Over the object's whole lifetime, exactly one thread calls push and
//   exactly one thread calls try_pop, and the destructor runs after both
//   have made their last call (ordered by a join or equivalent). The two
//   may be the same thread.
//
// What breaks if that fact is false:
//
//   A second producer races on the producer's private state and on the same
//   slot: elements are lost or duplicated and a slot holding a shared_ptr is
//   torn across its two words, so its control block is freed twice or never.
//   A second consumer moves the same slot out twice. A destructor that runs
//   under a live reader frees the block it is reading. None of this is
//   detected at runtime unless WTC_CHECK is defined, and TSan reports it only
//   on interleavings where it happens to occur.
//
// Semantics preserved from the generic queue: FIFO; push never blocks and
// never fails; try_pop never blocks and returns false only when the queue
// was empty at some instant during the call; no capacity.
//
// Design. Elements are numbered from zero. Blocks of kSlots slots are linked
// in a ring; slot i of a block holds element (block.end - kSlots + i). The
// producer owns its current block and the pointer out of it; the consumer
// owns its current block. Two counters cross between them, each on its own
// cache line and each written by one side only: push_pub_ is the number of
// elements published, pop_pub_ the number the consumer has finished with and
// left behind. Each side keeps a cached copy of the other's counter and reads
// the shared line only when the cached copy stops answering: the consumer
// when it looks empty, the producer once per block at turnover. Per element
// the cross-core traffic is the slot's own line plus, under load, a fraction
// of one line for push_pub_. The consumer performs no shared store per
// element at all. On x86 every atomic here is a plain move.
//
// A block is reused when the consumer has left it, which the consumer
// announces only after it has read the block's next pointer. That ordering
// is load-bearing: the producer inserts new blocks by writing next on its
// current block, and the consumer must never be positioned on that block
// when it does, or the insertion could reorder the ring under it. Blocks are
// never freed before the destructor; memory is monotone in the high-water
// mark of elements in flight, plus one block.
//
// Lineage: Lamport's ring (1983); FastForward, Giacomoni et al., PPoPP 2008,
// for the observation that the index lines are the bottleneck; Vyukov for
// the cached-index discipline and the reuse rule; Rigtorp's SPSCQueue for
// the line layout; moodycamel's ReaderWriterQueue for unboundedness through
// a ring of blocks. The two queue-global counters are what make this simpler
// than moodycamel: nothing is reset on reuse.
//
// Measured numbers against the general-purpose queue: see runtime/README.md,
// which is filled in from benchmarks/results/ and not from this comment.

#ifndef WTC_RUNTIME_SPSC_RING_HPP
#define WTC_RUNTIME_SPSC_RING_HPP

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <utility>

#ifdef WTC_CHECK
#include <pthread.h>
#endif

namespace wtc::rt {

// kSlots: measured, not chosen, and the measurement is in
// benchmarks/results under the w6 leg. Three things it showed, one of which
// corrected an earlier claim in this comment:
//
//   - 64 slots is too small and consistently slow, around 8 million
//     elements a second against 50 to 60 for every larger block.
//   - From 256 slots upward the median is flat to within about 15%, and
//     4096 sits at or near the top of that range.
//   - The ring has a second, much slower regime, and size makes it rarer
//     without abolishing it: at every block size from 256 up, a run in
//     twenty still lands there. This comment used to claim the fast regime
//     was the only one seen above 4096 slots, which was three repetitions
//     talking. The regime is what happens when the consumer settles in
//     immediately behind the producer, so each line the producer writes is
//     still live in the consumer's cache and has to be invalidated first.
//
// Padding the shared state to 128 bytes rather than 64 was tried, on the
// theory that this processor's prefetcher pairs adjacent lines. It made no
// difference beyond the noise, so kLine stays at the real line size.
//
// None of this matters for the shim, whose slots are shared pointers: there
// the allocator dominates and every block size measures the same, 3.2 to
// 3.4 million a second. It matters for a future slot type held inline. For
// the shim's slots 4096 costs 64 KiB per block, 128 KiB per queue at rest.
template <class Slot, unsigned kSlots = 4096, unsigned kLine = 64>
class spsc_ring {
  static_assert(kSlots >= 2, "a block needs at least two slots");
  static_assert((kLine & (kLine - 1)) == 0, "kLine must be a power of two");

  struct alignas(kLine) block {
    // Written by the producer only: once at construction and once per
    // insertion. Read by the consumer at turnover, after it has seen a
    // published count beyond this block.
    std::atomic<block*> next;
    // First element number not held by this block. Producer-private. Zero
    // means the block has never been filled, which reads as drained below.
    std::uint64_t end;
    // On its own line boundary, so a slot of one line's size occupies
    // exactly one line and no line is shared by two consecutive slots.
    // Without this, 64-byte elements straddled two lines each, both sides
    // touched every line, and throughput fell by forty times, measured.
    alignas(kLine) Slot slots[kSlots];

    block() : next(nullptr), end(0) {}
  };

  struct producer_state {
    block* blk;
    std::uint64_t count;      // elements pushed so far
    unsigned off;             // next free slot in blk
    std::uint64_t cached_pop; // last value read from pop_pub_
  };
  struct consumer_state {
    block* blk;
    std::uint64_t count;       // elements popped so far
    unsigned off;              // next slot to read in blk
    std::uint64_t cached_push; // last value read from push_pub_
  };

  alignas(kLine) producer_state p_;
  alignas(kLine) std::atomic<std::uint64_t> push_pub_;
  alignas(kLine) consumer_state c_;
  alignas(kLine) std::atomic<std::uint64_t> pop_pub_;
#ifdef WTC_CHECK
  alignas(kLine) std::atomic<std::uintptr_t> owner_p_;
  std::atomic<std::uintptr_t> owner_c_;
#endif
  char tail_pad_[kLine];

#ifdef WTC_CHECK
  static std::uintptr_t self() {
    thread_local const std::uintptr_t id =
        static_cast<std::uintptr_t>(pthread_self());
    return id;
  }
  void claim(std::atomic<std::uintptr_t>& owner, const char* role) {
    const std::uintptr_t me = self();
    std::uintptr_t seen = owner.load(std::memory_order_relaxed);
    if (seen == me) return;
    if (seen == 0 &&
        owner.compare_exchange_strong(seen, me, std::memory_order_relaxed))
      return;
    if (seen == me) return;
    std::fprintf(stderr,
                 "wtc: SPSC contract violated: a second %s thread on queue %p "
                 "(first %#lx, now %#lx)\n",
                 role, static_cast<void*>(this),
                 static_cast<unsigned long>(seen),
                 static_cast<unsigned long>(me));
    std::abort();
  }
#endif

  static block* make_block() { return new block(); }

public:
  spsc_ring() : push_pub_(0), pop_pub_(0)
#ifdef WTC_CHECK
                , owner_p_(0), owner_c_(0)
#endif
  {
    block* a = make_block();
    block* b = make_block();
    a->next.store(b, std::memory_order_relaxed);
    b->next.store(a, std::memory_order_relaxed);
    // The producer starts inside a without passing through turn_over, which
    // is where end is normally assigned. Without this line a keeps end == 0,
    // reads as drained one lap later, and is reused under the consumer.
    a->end = kSlots;
    p_ = producer_state{a, 0, 0, 0};
    c_ = consumer_state{a, 0, 0, 0};
  }

  spsc_ring(const spsc_ring&) = delete;
  spsc_ring& operator=(const spsc_ring&) = delete;

  // By contract, no other thread is inside push or try_pop now. Walking from
  // the consumer's block around the ring visits every block exactly once;
  // the slot destructors release whatever was still queued.
  ~spsc_ring() {
    block* start = c_.blk;
    block* b = start;
    do {
      block* n = b->next.load(std::memory_order_relaxed);
      delete b;
      b = n;
    } while (b != start);
  }

  // One copy or one move into the slot, never both: the argument goes
  // straight in by forwarding.
  template <class S>
  void push(S&& s) {
#ifdef WTC_CHECK
    claim(owner_p_, "producer");
#endif
    if (p_.off == kSlots) turn_over();
    p_.blk->slots[p_.off] = std::forward<S>(s);
    ++p_.off;
    ++p_.count;
    // Publishes the slot just written and, if turn_over inserted a block,
    // the next pointer that leads to it.
    push_pub_.store(p_.count, std::memory_order_release);
  }

  bool try_pop(Slot& out) {
#ifdef WTC_CHECK
    claim(owner_c_, "consumer");
#endif
    if (c_.count == c_.cached_push) {
      c_.cached_push = push_pub_.load(std::memory_order_acquire);
      if (c_.count == c_.cached_push) return false;
    }
    if (c_.off == kSlots) {
      // Element c_.count lives in the next block. Leave this one, then tell
      // the producer it may reuse everything below c_.count. The store must
      // follow the load of next: after it, the producer may write this
      // block's next pointer again.
      c_.blk = c_.blk->next.load(std::memory_order_acquire);
      c_.off = 0;
      pop_pub_.store(c_.count, std::memory_order_release);
    }
    out = std::move(c_.blk->slots[c_.off]);
    ++c_.off;
    ++c_.count;
    return true;
  }

  // Not synchronised; a hint for the owning threads only.
  std::uint64_t pushed() const { return p_.count; }
  std::uint64_t popped() const { return c_.count; }

private:
  void turn_over() {
    block* cand = p_.blk->next.load(std::memory_order_relaxed);
    // The candidate is drained when the consumer has announced that it left
    // every element the block holds. A never-filled block has end == 0.
    bool drained = cand->end <= p_.cached_pop;
    if (!drained) {
      // The one producer read of a consumer-written line, once per block.
      p_.cached_pop = pop_pub_.load(std::memory_order_acquire);
      drained = cand->end <= p_.cached_pop;
    }
    if (drained) {
      cand->end = p_.count + kSlots;
    } else {
      block* nb = make_block();
      nb->end = p_.count + kSlots;
      nb->next.store(cand, std::memory_order_relaxed);
      // The consumer reads this only after it has acquired a push_pub_
      // value beyond the current block, which the next push releases.
      p_.blk->next.store(nb, std::memory_order_release);
      cand = nb;
    }
    p_.blk = cand;
    p_.off = 0;
  }
};

} // namespace wtc::rt

#endif // WTC_RUNTIME_SPSC_RING_HPP
