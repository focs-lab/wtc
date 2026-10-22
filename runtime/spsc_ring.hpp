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
// Two ways to pop. try_pop moves the element out into the caller's object.
// try_pop_borrow leaves it where it is and returns its address: the
// element stays alive, in place, until the consumer's next try_pop or
// try_pop_borrow, or the destructor, and is destroyed then. That is what
// lets an element be held inline and handed over without a copy or an
// allocation; the price is the contract that the caller keeps no pointer
// into it past its next pop, which the compiler has to prove (with
// WTC_CHECK the slot is overwritten with 0xDB when it is released, so a
// pointer kept too long reads garbage rather than a plausible element).
// The lent slot is always in the consumer's current block, which the
// consumer has not announced as left, so the producer never reuses it.
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
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

#ifdef WTC_CHECK
#include <pthread.h>
#endif

namespace wtc::rt {

// A block is kBlockBytes of slots by default, whatever the slot: 16384
// slots of 8 bytes, 2048 of 64, 512 of 256, 8192 shared pointers. The size
// is measured, not chosen, and the measurement is in benchmarks/results
// under the w6 leg, which was run with the block in slots (4096 of 64
// bytes, 256 KiB) before it was set in bytes:
//
//   - 64 slots is too small and consistently slow, around 8 million
//     elements a second against 50 to 60 for every larger block.
//   - From 256 slots upward the median is flat to within about 15%.
//   - The ring has a second, much slower regime, and size makes it rarer
//     without abolishing it: at every block size from 256 up, a run in
//     twenty still lands there. The regime is what happens when the
//     consumer settles in immediately behind the producer, so each line the
//     producer writes is still live in the consumer's cache and has to be
//     invalidated first.
//
// 128 KiB keeps a block well clear of the 64 slots that are too few for
// any slot type, spends a quarter of what 4096 slots of 64 bytes did, and
// makes a block's turnover, the one moment the producer reads the
// consumer's line, rarer for the small elements that turn over most. Two
// blocks are allocated at rest.
//
// Padding the shared state to 128 bytes rather than 64 was tried, on the
// theory that this processor's prefetcher pairs adjacent lines. It made no
// difference beyond the noise, so kLine stays at the real line size.
//
// None of this matters for the shim's spsc queue, whose slots are shared
// pointers: there the allocator dominates and every block size measures the
// same, 3.2 to 3.4 million a second. It matters for elements held inline.
//
// kOneConsumer false is for spmc_ring, which lets consumers into try_pop
// one at a time under its own lock: the ring's consumer side is then still
// single-threaded, and only the contract check must not count them.
inline constexpr unsigned kBlockBytes = 128 * 1024;

template <class Slot>
constexpr unsigned default_slots() {
  constexpr unsigned n = kBlockBytes / sizeof(Slot);
  return n < 2 ? 2 : n;
}

template <class Slot, unsigned kSlots = default_slots<Slot>(), unsigned kLine = 64,
          bool kOneConsumer = true>
class spsc_ring {
  static_assert(kSlots >= 2, "a block needs at least two slots");
  static_assert((kLine & (kLine - 1)) == 0, "kLine must be a power of two");

  // Raw storage for one element: an element exists in a slot only between
  // the push that constructs it and the pop that destroys it, so a block
  // costs no constructor per slot and the element type needs no default
  // constructor. Same size and alignment as Slot, so the layout is that of
  // a Slot array.
  struct cell {
    alignas(Slot) unsigned char raw[sizeof(Slot)];
  };

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
    alignas(kLine) cell slots[kSlots];

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
    Slot* lent;                // the element try_pop_borrow handed out, if any
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
                 "wtc: %s contract violated: a second %s thread on queue %p "
                 "(first %#lx, now %#lx)\n",
                 kOneConsumer ? "SPSC" : "SPMC", role, static_cast<void*>(this),
                 static_cast<unsigned long>(seen),
                 static_cast<unsigned long>(me));
    std::abort();
  }
#endif

  static block* make_block() { return new block(); }

  static Slot* at(block* b, unsigned i) {
    return std::launder(reinterpret_cast<Slot*>(b->slots[i].raw));
  }

  // Ends the life of the element the last try_pop_borrow lent out.
  void release_lent() {
    if (!c_.lent) return;
    c_.lent->~Slot();
#ifdef WTC_CHECK
    std::memset(static_cast<void*>(c_.lent), 0xDB, sizeof(Slot));
#endif
    c_.lent = nullptr;
  }

  // The next element, or null if the queue was empty; advances past it.
  Slot* advance() {
    if (c_.count == c_.cached_push) {
      c_.cached_push = push_pub_.load(std::memory_order_acquire);
      if (c_.count == c_.cached_push) return nullptr;
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
    Slot* e = at(c_.blk, c_.off);
    ++c_.off;
    ++c_.count;
    return e;
  }

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
    c_ = consumer_state{a, 0, 0, 0, nullptr};
  }

  spsc_ring(const spsc_ring&) = delete;
  spsc_ring& operator=(const spsc_ring&) = delete;

  // By contract, no other thread is inside push or try_pop now. The elements
  // still queued are the ones from the consumer's position up to the
  // producer's, along the path the consumer would take; then walking from
  // the consumer's block around the ring visits every block exactly once.
  ~spsc_ring() {
    release_lent();
    if constexpr (!std::is_trivially_destructible_v<Slot>) {
      block* b = c_.blk;
      unsigned off = c_.off;
      for (std::uint64_t n = c_.count; n < p_.count; ++n) {
        if (off == kSlots) {
          b = b->next.load(std::memory_order_relaxed);
          off = 0;
        }
        at(b, off++)->~Slot();
      }
    }
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
    push_deferred(std::forward<S>(s));
    publish();
  }

  // push without publishing: the element is in its slot, but the consumer
  // cannot see it, or anything pushed after it, until publish. A run of
  // these followed by one publish costs one release store for the run
  // instead of one per element; the elements become visible together and
  // in order, which is what pushing them one by one with the consumer
  // looking away would have shown.
  template <class S>
  void push_deferred(S&& s) {
#ifdef WTC_CHECK
    claim(owner_p_, "producer");
#endif
    if (p_.off == kSlots) turn_over();
    ::new (static_cast<void*>(p_.blk->slots[p_.off].raw)) Slot(std::forward<S>(s));
    ++p_.off;
    ++p_.count;
  }

  // Publishes every element pushed so far and, if turn_over inserted a
  // block on the way, the next pointer that leads to it: the consumer reads
  // that pointer only after it has acquired a count beyond the block.
  void publish() { push_pub_.store(p_.count, std::memory_order_release); }

  bool try_pop(Slot& out) {
#ifdef WTC_CHECK
    if constexpr (kOneConsumer) claim(owner_c_, "consumer");
#endif
    release_lent();
    Slot* e = advance();
    if (!e) return false;
    out = std::move(*e);
    e->~Slot();
    return true;
  }

  // The next element in place, or null if the queue was empty. It stays
  // valid until this consumer's next pop of either kind, or the destructor.
  // The previously lent element is destroyed first, so it must not be
  // referenced any more when this is called.
  Slot* try_pop_borrow() {
    static_assert(kOneConsumer, "a borrowed element dies at the next pop of any consumer");
#ifdef WTC_CHECK
    claim(owner_c_, "consumer");
#endif
    release_lent();
    c_.lent = advance();
    return c_.lent;
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
