// Lemmas 1 to 3 of docs/correctness.md on the interleavings their sketches
// rely on, with blocks of four slots so that every test crosses many
// turnovers.
//
// Lemma 1, spsc_ring:
//   deferred_push_reads_empty  a push whose element is in its slot but not
//                              published: the pop answers empty, then
//                              finds it once published.
//   taker_holds_slot           the consumer is held inside a pop with the
//                              element still in its slot while the
//                              producer pushes many blocks: the slot is
//                              not reused under it, and FIFO holds after.
//   destroyed_with_elements    the destructor ends every element left.
//   stress (ring)              producer held at random, runs of deferred
//                              pushes, consumer held at random inside pops
//                              and mixing moves and borrows; every element
//                              once, in order, every empty answer checked.
// Lemma 2, borrowed elements:
//   borrow_survives_laps       a borrowed element, including the last slot
//                              of a block, stays intact while the producer
//                              laps the ring many times, and is destroyed
//                              at the next pop, once.
// Lemma 3, spmc_queue (segments of four cells):
//   spmc_no_wait_for_consumer  a consumer held inside its pop's taker holds
//                              no other consumer: another pop takes the
//                              next element and returns meanwhile, and the
//                              elements go in the order of the claims.
//   spmc_taker_holds_segment   a consumer held in its taker on the last
//                              cell of a segment while another consumer
//                              drains several segments past it, retiring
//                              that one: the segment is not freed under
//                              the taker, and the element is intact.
//   stress (spmc)              one producer, four consumers held at random
//                              inside pops; every element once, in order
//                              at each consumer, every empty answer
//                              checked.
#include <cstdint>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

#include "lemma_util.hpp"
#include "spmc_queue.hpp"
#include "spsc_ring.hpp"

namespace {

using lemma::item;
using ring = wtc::rt::spsc_ring<item, 4>;
using spmc = wtc::rt::spmc_queue<item, 4>;

void deferred_push_reads_empty() {
  {
    ring q;
    q.push_deferred(item(0, 1, 0));
    item v;
    EXPECT(!q.try_pop(v));  // not yet published: the push has not happened
    q.publish();
    EXPECT(q.try_pop(v) && v.seq == 0);
    EXPECT(!q.try_pop(v));
  }
  EXPECT(lemma::live.load() == 0);
}

void taker_holds_slot() {
  {
    ring q;  // one producer (this thread) and one consumer (c) throughout
    q.push(item(0, 1, 0));
    lemma::gate in_taker;
    long first_read = -1, second_read = -1;
    std::thread c([&] {
      q.consume([&](item&& e) noexcept {
        first_read = e.seq;
        in_taker.stop_here();
        second_read = e.seq;  // the slot, read again after the producer's laps
      });
      item v;
      for (long i = 1; i <= 80; ++i) EXPECT(q.try_pop(v) && v.seq == i);
      EXPECT(!q.try_pop(v));
    });
    in_taker.wait_arrival();
    for (long i = 1; i <= 80; ++i) q.push(item(0, 1, i));  // twenty blocks of four
    in_taker.release();
    c.join();
    EXPECT(first_read == 0 && second_read == 0);
  }
  EXPECT(lemma::live.load() == 0);
}

void destroyed_with_elements() {
  {
    ring q;
    for (long i = 0; i < 37; ++i) q.push(item(0, 1, i));
    item v;
    for (long i = 0; i < 13; ++i) EXPECT(q.try_pop(v));
    EXPECT(q.try_pop_borrow() != nullptr);  // and one lent
  }
  {
    spmc q;
    for (long i = 0; i < 21; ++i) q.push(item(0, 1, i));
    item v;
    EXPECT(q.try_pop(v));
  }
  EXPECT(lemma::live.load() == 0);
}

void borrow_survives_laps() {
  {
    ring q;  // one producer (prod) and one consumer (this thread) throughout
    lemma::gate first_four, laps;
    std::thread prod([&] {
      for (long i = 0; i < 4; ++i) q.push(item(0, 1, i));
      first_four.stop_here();
      for (long i = 4; i < 4 + 200; ++i) q.push(item(0, 1, i));  // fifty laps' worth
      laps.stop_here();
    });
    first_four.wait_arrival();
    item* p = nullptr;
    for (long i = 0; i < 4; ++i) p = q.try_pop_borrow();  // ends on the block's last slot
    EXPECT(p && p->seq == 3);
    first_four.release();
    laps.wait_arrival();
    EXPECT(p->seq == 3);  // still the lent element
    const long before = lemma::live.load();
    item* n = q.try_pop_borrow();
    EXPECT(lemma::live.load() == before - 1);  // the lent one ended, once
    EXPECT(n && n->seq == 4);
    item v;
    for (long i = 5; i < 204; ++i) EXPECT(q.try_pop(v) && v.seq == i);
    laps.release();
    prod.join();
  }
  EXPECT(lemma::live.load() == 0);
}

// Waits up to five seconds for done; true if it was set.
bool finishes(const std::atomic<bool>& done) {
  for (int i = 0; i < 500; ++i) {
    if (done.load(std::memory_order_acquire)) return true;
    lemma::sleep_ms(10);
  }
  return false;
}

void spmc_no_wait_for_consumer() {
  {
    spmc q;
    q.push(item(0, 1, 0));
    q.push(item(0, 1, 1));
    lemma::gate in_taker;
    long a_seq = -1;
    std::thread a([&] {
      q.consume([&](item&& e) noexcept {
        a_seq = e.seq;
        in_taker.stop_here();
      });
    });
    in_taker.wait_arrival();
    std::atomic<bool> b_done{false};
    item b_out;
    std::thread b([&] {
      EXPECT(q.try_pop(b_out));
      b_done.store(true, std::memory_order_release);
    });
    EXPECT(finishes(b_done));  // does not wait for the other consumer
    in_taker.release();
    a.join();
    b.join();
    EXPECT(a_seq == 0 && b_out.seq == 1);
  }
  EXPECT(lemma::live.load() == 0);
}

void spmc_taker_holds_segment() {
  {
    spmc q;
    for (long i = 0; i < 40; ++i) q.push(item(0, 1, i));
    item v;
    for (long i = 0; i < 3; ++i) EXPECT(q.try_pop(v) && v.seq == i);
    lemma::gate in_taker;
    long a_seq = -1;
    std::thread a([&] {
      q.consume([&](item&& e) noexcept {
        in_taker.stop_here();
        a_seq = e.seq;  // read after the others have moved on
      });
    });
    in_taker.wait_arrival();
    std::atomic<bool> b_done{false};
    std::thread b([&] {
      item w;
      for (long i = 4; i < 40; ++i) EXPECT(q.try_pop(w) && w.seq == i);
      EXPECT(!q.try_pop(w));
      b_done.store(true, std::memory_order_release);
    });
    EXPECT(finishes(b_done));
    in_taker.release();
    a.join();
    b.join();
    EXPECT(a_seq == 3);
  }
  EXPECT(lemma::live.load() == 0);
}

void ring_stress(long n) {
  {
    ring q;
    lemma::empty_check check;
    std::thread prod([&] {
      std::mt19937 r(1);
      lemma::move_jitter = 5;
      long s = 0;
      while (s < n) {
        if (r() % 5 == 0) {
          long k = 1 + r() % 9, j = 0;
          for (; j < k && s < n; ++j) q.push_deferred(item(0, 1, s++));
          q.publish();
          check.pushed(j);
        } else {
          q.push(item(0, 1, s++));
          check.pushed();
        }
        if (r() % 101 == 0) std::this_thread::yield();
      }
    });
    std::mt19937 r(2);
    long next = 0;
    while (next < n) {
      const unsigned how = r() % 3;
      bool got = false;
      long seen = -1;
      if (how == 0) {
        item v;
        got = check.pop([&] { return q.try_pop(v); });
        seen = v.seq;
      } else if (how == 1) {
        got = check.pop([&] {
          return q.consume([&](item&& e) noexcept {
            seen = e.seq;
            if ((e.seq & 255) == 0) std::this_thread::yield();
          });
        });
      } else {
        item* p = nullptr;
        got = check.pop([&] { return (p = q.try_pop_borrow()) != nullptr; });
        if (got) seen = p->seq;
      }
      if (got) {
        EXPECT(seen == next);
        ++next;
      }
    }
    prod.join();
    EXPECT(check.violations.load() == 0);
  }
  EXPECT(lemma::live.load() == 0);
}

void spmc_stress(long n) {
  const int consumers = 4;
  {
    spmc q;
    lemma::empty_check check;
    std::atomic<bool> done{false};
    std::atomic<long> popped{0};
    std::vector<std::thread> ts;
    ts.emplace_back([&] {
      std::mt19937 r(3);
      lemma::move_jitter = 9;
      for (long s = 0; s < n;) {
        if (r() % 5 == 0) {
          long k = 1 + r() % 9, j = 0;
          for (; j < k && s < n; ++j) q.push_deferred(item(0, 1, s++));
          q.publish();
          check.pushed(j);
        } else {
          q.push(item(0, 1, s++));
          check.pushed();
        }
      }
      done.store(true);
    });
    for (int c = 0; c < consumers; ++c)
      ts.emplace_back([&, c] {
        std::mt19937 r(10 + c);
        long last = -1;
        for (;;) {
          long seen = -1;
          bool got = check.pop([&] {
            return q.consume([&](item&& e) noexcept {
              seen = e.seq;
              if (r() % 211 == 0) std::this_thread::yield();
            });
          });
          if (got) {
            EXPECT(seen > last);
            last = seen;
            popped.fetch_add(1);
          } else if (done.load() && popped.load() == n) {
            break;
          }
        }
      });
    for (auto& t : ts) t.join();
    EXPECT(popped.load() == n);
    EXPECT(check.violations.load() == 0);
  }
  EXPECT(lemma::live.load() == 0);
}

}  // namespace

int main(int argc, char** argv) {
  const long n = argc > 1 ? std::atol(argv[1]) : 200000;
  deferred_push_reads_empty();
  taker_holds_slot();
  destroyed_with_elements();
  borrow_survives_laps();
  spmc_no_wait_for_consumer();
  spmc_taker_holds_segment();
  ring_stress(n);
  spmc_stress(n);
  if (lemma::failures.load()) {
    std::fprintf(stderr, "lemma_rings_test: %d failure(s)\n", lemma::failures.load());
    return 1;
  }
  std::puts("lemma_rings_test: ok");
  return 0;
}
