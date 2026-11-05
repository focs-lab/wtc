// wtc::rt::recycle_ring: objects handed back from a queue's consumer to its
// producer, so that the producer reuses them instead of allocating.
//
// The fact the compiler must prove before replacing an allocation with
// take and a deallocation with give:
//
//   The queue has exactly one producer and one consumer. Every object the
//   producer pushes comes from the one allocation that take replaces, made
//   on the producer's thread, of the same size each time; every object the
//   consumer pops reaches the one deallocation that give replaces, on the
//   consumer's thread, and nothing keeps a pointer to it after that. So
//   exactly one thread calls take and exactly one calls give, every block
//   given back is one taken from this ring or from the same allocator, and
//   the destructor runs after both have made their last call.
//
// What breaks if that fact is false:
//
//   An object given back while something still points to it is reused
//   under that pointer: the producer's next object and the stale reader
//   share memory, and each sees the other's writes. A second giver or
//   taker races on the ring's indices: a block is handed out twice, or
//   never, and the same memory becomes two live objects. A block of
//   another size, or from another allocator, is handed out as if it were
//   this ring's. None of this is detected at runtime unless WTC_CHECK is
//   defined (a second giver or taker, or a second size, aborts), and TSan
//   reports it only on interleavings where it happens to occur.
//
// Semantics. The program's own allocation and deallocation are kept
// whenever the ring cannot serve: take allocates when the ring is empty,
// give frees when it is full, each with the allocator the program named
// (recycle_kind). A block that comes from the ring is the memory a freed
// object occupied; for recycle_kind::calloc_ it is zeroed first, so that
// the guarantee calloc makes still holds. Nothing else about the memory is
// promised, as nothing is about fresh memory from malloc or operator new.
// Constructors and destructors are the program's and still run: only the
// raw allocation and deallocation are replaced.
//
// Ordering. give stores the block in its slot and publishes it with a
// release store of the count given; take reads that count with an acquire
// load before it reads the slot. So everything the consumer did with the
// object before giving it back happens before everything the producer
// does with it after taking it: the reuse is ordered as a free followed by
// a malloc that returned the same address would be. take publishes the
// count taken the same way, so the consumer reuses a slot only after the
// producer has read it out.
//
// Capacity. kCapacity blocks (1024): a consumer that falls far behind, or
// one that frees in bursts, gives back at most that many, and the rest go
// to the allocator. Memory held by the ring goes back to the allocator
// only when the ring is destroyed.
//
// Measured: not yet. The cost it removes is an allocation on the producer
// and a free on the consumer per element; on the 28 October ablation an
// element the producer allocates and the consumer frees cost the ring 7.7×
// (benchmarks/results/2026-10-28-eval-ablation, spsc_xsp against
// spsc_cell), less under snmalloc and for large elements. W13 measures the
// ring itself against that and against a pool written by hand.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace wtc::rt {

// The allocator a recycled object's memory comes from and goes back to. The
// compiler passes it as a constant, read from the allocation it replaces.
enum class recycle_kind : int {
  malloc_ = 0,  // malloc; free
  calloc_ = 1,  // calloc of one element; free; zeroed when reused
  new_ = 2,     // the global operator new; operator delete
};

class recycle_ring {
 public:
  static constexpr std::size_t kCapacity = 1024;

  recycle_ring() = default;
  recycle_ring(const recycle_ring&) = delete;
  recycle_ring& operator=(const recycle_ring&) = delete;

  // Every block still in the ring goes back to its allocator.
  ~recycle_ring() {
    const std::uint64_t given = given_.load(std::memory_order_relaxed);
    for (std::uint64_t i = taken_.load(std::memory_order_relaxed); i < given; ++i)
      release(slots_[i % kCapacity], kind_);
  }

  // The producer's next block of `size` bytes: one the consumer gave back,
  // or a fresh one from the allocator when there is none. Null only when
  // the allocator fails, as malloc's would be (operator new throws).
  void* take(std::size_t size, recycle_kind kind) {
#ifdef WTC_CHECK
    claim(taker_, "taker");
    check_size(size);
#endif
    kind_ = kind;
    const std::uint64_t t = p_taken_;
    if (t == p_given_) {
      p_given_ = given_.load(std::memory_order_acquire);
      if (t == p_given_) return allocate(size, kind);
    }
    void* p = slots_[t % kCapacity];
    p_taken_ = t + 1;
    taken_.store(t + 1, std::memory_order_release);
    if (kind == recycle_kind::calloc_) std::memset(p, 0, size);
    return p;
  }

  // The consumer's block, back to the producer; to the allocator when the
  // ring is full. A null block is ignored, as free and delete ignore it.
  void give(void* p, recycle_kind kind) {
    if (!p) return;
#ifdef WTC_CHECK
    claim(giver_, "giver");
#endif
    const std::uint64_t g = c_given_;
    if (g - c_taken_ == kCapacity) {
      c_taken_ = taken_.load(std::memory_order_acquire);
      if (g - c_taken_ == kCapacity) {
        release(p, kind);
        return;
      }
    }
    slots_[g % kCapacity] = p;
    c_given_ = g + 1;
    given_.store(g + 1, std::memory_order_release);
  }

  // The allocator itself, for a block the ring does not serve.
  static void* allocate(std::size_t size, recycle_kind kind) {
    switch (kind) {
    case recycle_kind::calloc_: return std::calloc(1, size);
    case recycle_kind::new_: return ::operator new(size);
    default: return std::malloc(size);
    }
  }
  static void release(void* p, recycle_kind kind) {
    if (kind == recycle_kind::new_) ::operator delete(p);
    else std::free(p);
  }

 private:
  static constexpr std::size_t kLine = 64;

#ifdef WTC_CHECK
  // A thread's identity: the address of a thread-local, which no two live
  // threads share. pthread_t is an integer on Linux and a pointer on macOS.
  static std::uintptr_t self() {
    thread_local char tag;
    return reinterpret_cast<std::uintptr_t>(&tag);
  }
  void claim(std::atomic<std::uintptr_t>& owner, const char* role) {
    const std::uintptr_t me = self();
    std::uintptr_t seen = owner.load(std::memory_order_relaxed);
    if (seen == me) return;
    if (seen == 0 && owner.compare_exchange_strong(seen, me, std::memory_order_relaxed)) return;
    if (seen == me) return;
    std::fprintf(stderr,
                 "wtc: recycle contract violated: a second %s thread on ring %p "
                 "(first %#lx, now %#lx)\n",
                 role, static_cast<void*>(this), static_cast<unsigned long>(seen),
                 static_cast<unsigned long>(me));
    std::abort();
  }
  void check_size(std::size_t size) {
    if (size_ == 0) size_ = size;
    if (size_ == size) return;
    std::fprintf(stderr,
                 "wtc: recycle contract violated: blocks of %zu and %zu bytes on ring %p\n",
                 size_, size, static_cast<void*>(this));
    std::abort();
  }
#endif

  // The taker's: its own count and its copy of the giver's.
  alignas(kLine) std::uint64_t p_taken_ = 0;
  std::uint64_t p_given_ = 0;
  // The allocator, for the destructor: the taker writes it, and the
  // destructor runs after the taker's last call.
  recycle_kind kind_ = recycle_kind::malloc_;
#ifdef WTC_CHECK
  std::size_t size_ = 0;
#endif
  // The giver's: its own count and its copy of the taker's.
  alignas(kLine) std::uint64_t c_given_ = 0;
  std::uint64_t c_taken_ = 0;
  // Each written by one side only, on a line of its own.
  alignas(kLine) std::atomic<std::uint64_t> given_{0};
  alignas(kLine) std::atomic<std::uint64_t> taken_{0};
#ifdef WTC_CHECK
  alignas(kLine) std::atomic<std::uintptr_t> taker_{0};
  std::atomic<std::uintptr_t> giver_{0};
#endif
  alignas(kLine) void* slots_[kCapacity] = {};
};

}  // namespace wtc::rt
