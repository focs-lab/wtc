// Tests of the return ring (runtime/recycle_ring.hpp).
//
//   recycle_ring_test unit         one thread: an empty ring allocates, a
//                                  block given back is taken next and in
//                                  order, a full ring frees what it cannot
//                                  hold, calloc's blocks come back zeroed,
//                                  a null block is ignored
//   recycle_ring_test stress [N]   a producer takes each object from the
//                                  ring, fills it and pushes the pointer
//                                  through a ring to a consumer, which
//                                  checks it, writes over it as a program
//                                  that uses its object would, and gives it
//                                  back; for each allocator kind
//   recycle_ring_test twogivers    two consumers giving back to one ring,
//                                  which the contract check must catch
//   recycle_ring_test shim [N]     the same hand-back through the shim's
//                                  wtc::queue<T*> (recycle_take and
//                                  recycle_give, as the compiler calls
//                                  them): round the ring on the spsc and
//                                  spsc_inline tags, to the allocator on
//                                  the others, and a stress run on each
//
// Run it under ThreadSanitizer as well. The stress mode is the one that
// matters there: the consumer's writes to an object and the producer's
// writes after reusing it touch the same memory, and only the ring's
// release and acquire order them. With two givers TSan must report a race,
// which is the test that the test can fail.

#include "../../shim/queue.hpp"
#include "recycle_ring.hpp"
#include "spsc_ring.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

using wtc::rt::recycle_kind;
using wtc::rt::recycle_ring;

int failures = 0;

void expect(bool ok, const char* what) {
  if (ok) return;
  std::fprintf(stderr, "FAIL: %s\n", what);
  ++failures;
}

struct object {
  std::uint64_t seq;
  std::uint64_t words[7];
};

void unit() {
  for (recycle_kind kind : {recycle_kind::malloc_, recycle_kind::calloc_, recycle_kind::new_}) {
    recycle_ring r;
    void* fresh = r.take(sizeof(object), kind);
    expect(fresh != nullptr, "an empty ring allocates");
    r.give(fresh, kind);
    expect(r.take(sizeof(object), kind) == fresh, "a block given back is taken next");

    // FIFO, and only kCapacity of them: the one past capacity goes back to
    // the allocator, so the ring hands out exactly the first kCapacity.
    std::vector<void*> given;
    given.push_back(fresh);
    while (given.size() < recycle_ring::kCapacity + 1) given.push_back(r.take(sizeof(object), kind));
    for (void* p : given) r.give(p, kind);
    bool fifo = true;
    for (std::size_t i = 0; i < recycle_ring::kCapacity; ++i)
      if (r.take(sizeof(object), kind) != given[i]) fifo = false;
    expect(fifo, "the ring hands blocks back in the order they were given");
    // Every block of `given` but the last is now out again; give them back
    // so that the destructor has a full ring to free.
    for (std::size_t i = 0; i < recycle_ring::kCapacity; ++i) r.give(given[i], kind);

    r.give(nullptr, kind);
    if (kind == recycle_kind::calloc_) {
      // Dirty a block, give it back, and take it again: calloc's promise.
      recycle_ring z;
      auto* o = static_cast<object*>(z.take(sizeof(object), kind));
      std::memset(o, 0xAB, sizeof *o);
      z.give(o, kind);
      auto* again = static_cast<object*>(z.take(sizeof(object), kind));
      expect(again == o, "the dirty block is the one taken back");
      bool zero = again->seq == 0;
      for (std::uint64_t w : again->words) zero = zero && w == 0;
      expect(zero, "a calloc block taken from the ring is zeroed");
      z.give(again, kind);
    }
  }
}

void stress(std::uint64_t n, recycle_kind kind) {
  recycle_ring returns;
  wtc::rt::spsc_ring<object*, 64> forward;
  std::atomic<bool> bad{false};

  std::thread consumer([&] {
    for (std::uint64_t expected = 0; expected < n;) {
      object* o;
      if (!forward.try_pop(o)) continue;
      bool ok = o->seq == expected;
      for (unsigned i = 0; i < 7; ++i) ok = ok && o->words[i] == expected * 7 + i;
      if (!ok) bad.store(true, std::memory_order_relaxed);
      // The consumer's own use of the object: it writes over it before
      // giving it back, and the producer's next writes must come after.
      o->seq = ~std::uint64_t(0);
      for (std::uint64_t& w : o->words) w = 0xDEADBEEF;
      returns.give(o, kind);
      ++expected;
    }
  });
  for (std::uint64_t i = 0; i < n; ++i) {
    auto* o = static_cast<object*>(returns.take(sizeof(object), kind));
    if (kind == recycle_kind::calloc_ && (o->seq != 0 || o->words[0] != 0))
      bad.store(true, std::memory_order_relaxed);
    o->seq = i;
    for (unsigned k = 0; k < 7; ++k) o->words[k] = i * 7 + k;
    forward.push(o);
  }
  consumer.join();
  expect(!bad.load(), "every object arrives whole and in order");
}

// Two consumers give back blocks of their own to one ring.
void twogivers(std::uint64_t n) {
  recycle_ring r;
  auto give = [&] {
    for (std::uint64_t i = 0; i < n; ++i) r.give(std::malloc(sizeof(object)), recycle_kind::malloc_);
  };
  std::thread a(give), b(give);
  a.join();
  b.join();
}

// What the compiler's rewrite of `new object` and `delete o` calls, on a
// wtc::queue<object*> built with each tag.
void shim(std::uint64_t n) {
  constexpr int kNew = static_cast<int>(recycle_kind::new_);
  for (wtc::topology tag : {wtc::topology::spsc, wtc::topology::spsc_inline, wtc::topology::generic,
                            wtc::topology::mpsc, wtc::topology::spmc}) {
    const bool rings = tag == wtc::topology::spsc || tag == wtc::topology::spsc_inline;
    {
      // One thread: two objects handed over and given back come round
      // again in order on a ring; elsewhere they went to the allocator.
      wtc::queue<object*> q(tag);
      void* a = q.recycle_take(sizeof(object), kNew);
      void* b = q.recycle_take(sizeof(object), kNew);
      q.push(static_cast<object*>(a));
      q.push(static_cast<object*>(b));
      object* o;
      while (q.try_pop(o)) q.recycle_give(o, kNew);
      void* a2 = q.recycle_take(sizeof(object), kNew);
      void* b2 = q.recycle_take(sizeof(object), kNew);
      if (rings) expect(a2 == a && b2 == b, "a ring tag hands the objects back in order");
      q.recycle_give(a2, kNew);
      q.recycle_give(b2, kNew);
      q.recycle_give(nullptr, kNew);
    }
    // Two threads, the consumer writing over each object before giving it
    // back; the queue's destructor frees what the ring still holds.
    wtc::queue<object*> q(tag);
    std::atomic<bool> bad{false};
    std::thread consumer([&] {
      for (std::uint64_t expected = 0; expected < n;) {
        object* o;
        if (!q.try_pop(o)) continue;
        if (o->seq != expected || o->words[6] != expected * 7 + 6) bad.store(true, std::memory_order_relaxed);
        o->seq = ~std::uint64_t(0);
        for (std::uint64_t& w : o->words) w = 0xDEADBEEF;
        q.recycle_give(o, kNew);
        ++expected;
      }
    });
    for (std::uint64_t i = 0; i < n; ++i) {
      auto* o = static_cast<object*>(q.recycle_take(sizeof(object), kNew));
      o->seq = i;
      for (unsigned k = 0; k < 7; ++k) o->words[k] = i * 7 + k;
      q.push(o);
    }
    consumer.join();
    expect(!bad.load(), "through the shim, every object arrives whole and in order");
  }
}

std::uint64_t count_arg(int argc, char** argv, std::uint64_t dflt) {
  return argc > 2 ? std::strtoull(argv[2], nullptr, 10) : dflt;
}

}  // namespace

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "unit";
  if (!std::strcmp(mode, "unit")) {
    unit();
  } else if (!std::strcmp(mode, "stress")) {
    const std::uint64_t n = count_arg(argc, argv, 2'000'000);
    for (recycle_kind kind : {recycle_kind::malloc_, recycle_kind::calloc_, recycle_kind::new_}) stress(n, kind);
  } else if (!std::strcmp(mode, "shim")) {
    shim(count_arg(argc, argv, 500'000));
  } else if (!std::strcmp(mode, "twogivers")) {
    twogivers(count_arg(argc, argv, 200'000));
  } else {
    std::fprintf(stderr, "usage: recycle_ring_test unit | stress [N] | shim [N] | twogivers [N]\n");
    return 2;
  }
  if (failures) return 1;
  std::printf("recycle_ring_test %s: ok\n", mode);
  return 0;
}
