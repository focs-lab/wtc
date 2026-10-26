// wtc::rt::spmc_ring: an unbounded queue for one producer and any number of
// consumers.
//
// The fact the compiler must prove before substituting this for wtc::queue:
//
//   Over the object's whole lifetime exactly one thread calls push (or
//   push_deferred and publish), any number call try_pop, and the
//   destructor runs after all of them have made their last call (ordered
//   by a join or equivalent).
//
// What breaks if that fact is false: what breaks for spsc_ring with two
// producers -- elements lost or duplicated, a shared_ptr slot torn across
// its two words. Nothing is detected at runtime unless WTC_CHECK is
// defined, and TSan reports it only on interleavings where it occurs.
//
// Semantics preserved from the generic queue: FIFO in the order of the
// pushes; push never blocks and never fails; no capacity; try_pop returns
// false only when every element published so far has been popped. try_pop
// may wait for another consumer's pop, which the generic queue's never
// does: for its turn, which includes the taker it was given, so the move of
// the element out of its slot and, through the shim's shared_ptr pop, an
// allocation (docs/correctness.md, section 7).
//
// Design: spsc_ring with its consumer side behind a spin lock. The
// producer's side is the ring's, unchanged: a store into a slot and one
// release store per push or per run, no lock, no read-modify-write, no
// allocation but a block when the ring is full. Consumers take turns
// through the ring's try_pop, which moves the element out and destroys the
// slot before the lock is let go: the ring tells the producer a block may
// be reused once its consumer side has left it, and with several consumers
// that must still mean no consumer is reading any slot in it.
//
// Compared with the generic queue it replaces: no mutex on the producer's
// side, which the generic queue's consumers also take to compare the head
// with the tail, and no node allocated per element.
//
// The shim instantiates it with Slot = T, the element by value. Elements
// are never lent: a borrowed element would die at the next pop of any
// consumer, which no consumer can see coming.
#pragma once

#include <atomic>
#include <thread>
#include <type_traits>
#include <utility>

#include "spsc_ring.hpp"

namespace wtc::rt {

template <class Slot, unsigned kSlots = default_slots<Slot>(), unsigned kLine = 64>
class spmc_ring {
 public:
  spmc_ring() = default;
  spmc_ring(const spmc_ring&) = delete;
  spmc_ring& operator=(const spmc_ring&) = delete;

  template <class S>
  void push(S&& s) { ring_.push(std::forward<S>(s)); }

  template <class S>
  void push_deferred(S&& s) { ring_.push_deferred(std::forward<S>(s)); }

  void publish() { ring_.publish(); }

  template <class F>
  bool consume(F&& f) {
    static_assert(std::is_nothrow_invocable_v<F, Slot&&>, "the taker must not throw: it runs under the lock");
    lock();
    const bool got = ring_.consume(f);
    unlock();
    return got;
  }

  bool try_pop(Slot& out) {
    return consume([&](Slot&& s) noexcept { out = std::move(s); });
  }

 private:
  void lock() {
    for (unsigned spins = 0; busy_.exchange(true, std::memory_order_acquire);) {
      while (busy_.load(std::memory_order_relaxed)) {
        if (++spins < 128) relax();
        else std::this_thread::yield();
      }
    }
  }
  void unlock() { busy_.store(false, std::memory_order_release); }

  static void relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
  }

  spsc_ring<Slot, kSlots, kLine, false> ring_;
  alignas(kLine) std::atomic<bool> busy_{false};
  char tail_pad_[kLine];
};

}  // namespace wtc::rt
