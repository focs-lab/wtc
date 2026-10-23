// The shim's queue for many producers and one consumer from 21 to 22 Oct
// 2026, kept as a baseline (orig_mpsc) so that the leg that measured it
// (eval-w9 of 21 Oct) stays reproducible; the shim's mpsc tag is the
// segmented queue's single-consumer form since. The file is the runtime's
// mpsc_list.hpp as it was, moved here unchanged.
//
// wtc::rt::mpsc_list: an unbounded queue for any number of producers and
// one consumer.
//
// The fact the compiler must prove before substituting this for wtc::queue:
//
//   Over the object's whole lifetime exactly one thread calls try_pop, any
//   number call push, and the destructor runs after all of them have made
//   their last call (ordered by a join or equivalent).
//
// What breaks if that fact is false:
//
//   Two consumers read the same node's successor, both move its element out
//   and both delete the node they leave: an element is delivered twice and
//   a node is freed twice. Nothing is detected at runtime unless WTC_CHECK
//   is defined, and TSan reports it only on interleavings where it occurs.
//
// Semantics preserved from the generic queue: FIFO in the order the pushes
// took effect, so an element a producer pushed before it signalled another
// producer comes out before anything that one pushes afterwards; push never
// blocks and never fails; no capacity. try_pop never blocks. It may return
// false while a push that started earlier is between its two steps, even if
// later pushes have finished: the elements behind an unfinished push stay
// out of reach until it finishes, which is what keeps the order. A consumer
// that spins on try_pop, as every user of the shim does, sees them a moment
// later.
//
// Design: the intrusive node-based MPSC queue ("Intrusive MPSC node-based
// queue", 1024cores). A list from the consumer's head, a node whose element
// has already been taken, to the producers' tail. A push makes a node,
// swaps it into the tail with one exchange, and links the previous tail to
// it; a pop reads the head's successor, takes its element, and frees the
// old head. The consumer frees a node only once it has moved past it, which
// needs the node's successor link, which the one producer that swapped that
// node out writes: so no producer ever touches a freed node. A run of
// pushes is linked privately and spliced in with one exchange.
//
// The shim instantiates it with Slot = std::shared_ptr<T>, as it does the
// ring: one node per element besides the element's own allocation, and no
// mutex on either side.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>

#ifdef WTC_CHECK
#include <pthread.h>
#endif

namespace wtc::rt {

template <class Slot, unsigned kLine = 64>
class mpsc_list {
  struct node {
    std::atomic<node*> next{nullptr};
    Slot value{};
  };

 public:
  // A producer's run of pushes, linked privately until publish.
  struct chain {
    node* first = nullptr;
    node* last = nullptr;
  };

  mpsc_list() {
    node* stub = new node;
    head_ = stub;
    tail_.store(stub, std::memory_order_relaxed);
  }

  ~mpsc_list() {
    for (node* n = head_; n;) {
      node* next = n->next.load(std::memory_order_relaxed);
      delete n;
      n = next;
    }
  }

  mpsc_list(const mpsc_list&) = delete;
  mpsc_list& operator=(const mpsc_list&) = delete;

  template <class S>
  void push(S&& s) {
    node* n = new node;
    n->value = std::forward<S>(s);
    splice(n, n);
  }

  template <class S>
  void push_deferred(chain& c, S&& s) {
    node* n = new node;
    n->value = std::forward<S>(s);
    if (c.last) c.last->next.store(n, std::memory_order_relaxed);
    else c.first = n;
    c.last = n;
  }

  void publish(chain& c) {
    if (!c.first) return;
    splice(c.first, c.last);
    c.first = c.last = nullptr;
  }

  bool try_pop(Slot& out) {
#ifdef WTC_CHECK
    claim_consumer();
#endif
    node* next = head_->next.load(std::memory_order_acquire);
    if (!next) return false;
    out = std::move(next->value);
    next->value = Slot{};
    delete head_;
    head_ = next;
    return true;
  }

 private:
  // The one step that makes nodes reachable: swap the tail, then link the
  // old one. Between the two, the consumer stops at the old tail.
  void splice(node* first, node* last) {
    node* prev = tail_.exchange(last, std::memory_order_acq_rel);
    prev->next.store(first, std::memory_order_release);
  }

#ifdef WTC_CHECK
  void claim_consumer() {
    thread_local const std::uintptr_t me = static_cast<std::uintptr_t>(pthread_self());
    std::uintptr_t seen = consumer_.load(std::memory_order_relaxed);
    if (seen == me) return;
    if (seen == 0 && consumer_.compare_exchange_strong(seen, me, std::memory_order_relaxed)) return;
    if (seen == me) return;
    std::fprintf(stderr,
                 "wtc: MPSC contract violated: a second consumer thread on queue %p "
                 "(first %#lx, now %#lx)\n",
                 static_cast<void*>(this), static_cast<unsigned long>(seen),
                 static_cast<unsigned long>(me));
    std::abort();
  }
#endif

  alignas(kLine) std::atomic<node*> tail_;   // producers
  alignas(kLine) node* head_;                // the consumer's
#ifdef WTC_CHECK
  alignas(kLine) std::atomic<std::uintptr_t> consumer_{0};
#endif
  char tail_pad_[kLine];
};

}  // namespace wtc::rt
