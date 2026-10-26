// Lemma 4 of docs/correctness.md for the multi-consumer segmented queue,
// the general-purpose implementation, on the interleavings its sketch
// relies on, and the single-consumer form under the same stress. The
// single-consumer form's interleavings are segment_queue_empty_test's.
//
//   held_push_is_waited_for  a push holds its cell between taking it and
//                            filling it while a later push finishes: a
//                            pop does not answer empty, it waits.
//   linearised_at_the_take   the pop that waits returns the held element
//                            although a later push filled its cell first:
//                            pushes are ordered by the cell they take,
//                            not by when they fill it.
//   pop_waits_for_late_push  a pop takes a cell no push has taken yet and
//                            returns the push that takes it meanwhile,
//                            while another pop answers empty.
//   batch_torn_by_a_skip     a demonstration rather than an obligation: a
//                            pop skips a cell inside a batch's range before
//                            the batch takes it; the batch keeps its order
//                            (C3) and another push lands between its parts,
//                            which C3 allows but does not require.
//   segment_kept_for_reader  a pop held between taking the last cell of a
//                            segment and reading it, while other threads
//                            move the head on and retire many segments.
//   stress                   producers held at random in a move, batches,
//                            consumers held at random at the hook points;
//                            every element once, each producer's elements
//                            in order at each consumer, every empty answer
//                            checked; four consumers, then one with the
//                            single-consumer form.
#include <cstdint>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

#include "lemma_util.hpp"
#include "segment_queue.hpp"

namespace {
// The pop hook: a thread holds at a stage when it has a gate for it.
thread_local lemma::gate* t_hold[2] = {nullptr, nullptr};
thread_local unsigned t_random_hold = 0;  // stress: hold now and then, by this seed
void jitter();
void pop_hook(unsigned stage) {
  if (lemma::gate* g = t_hold[stage]) {
    t_hold[stage] = nullptr;
    g->stop_here();
    return;
  }
  jitter();
}
void jitter() {
  if (t_random_hold) {
    t_random_hold = t_random_hold * 1103515245u + 12345u;
    if ((t_random_hold >> 16) % 61 == 0) std::this_thread::yield();
  }
}

// The queue's hook points (segment_queue.hpp, no_hooks): the pop stages
// hold a thread that has a gate for them; every point yields at random in
// the stress runs.
struct hooks : wtc::rt::no_hooks {
  static void pop_stage(unsigned stage) { pop_hook(stage); }
  static void look() { jitter(); }
  static void between_looks() { jitter(); }
};

using lemma::item;
using queue = wtc::rt::segment_queue<item, true, 8, 64, hooks>;  // eight cells a segment: many turnovers

void held_push_is_waited_for() {
  {
    queue q;
    lemma::arm_stall(1, 1);  // the move into the cell
    std::thread p([&] {
      item a(1, 1, 0);
      q.push_run(&a, 1);
    });
    lemma::stall_gate.wait_arrival();
    q.push(item(0, 2, 0));  // finishes, in the cell after the held one

    std::atomic<bool> done{false};
    bool got = false;
    item out;
    std::thread c([&] {
      got = q.try_pop(out);
      done.store(true, std::memory_order_release);
    });
    EXPECT(lemma::still_running(done));  // waits; an empty answer would be wrong
    lemma::stall_gate.release();
    p.join();
    c.join();
    EXPECT(got);
    EXPECT(out.producer == 1);
    item rest;
    EXPECT(q.try_pop(rest) && rest.producer == 2);
    EXPECT(!q.try_pop(rest));
  }
  EXPECT(lemma::live.load() == 0);
}

void linearised_at_the_take() {
  {
    queue q;
    lemma::arm_stall(1, 1);
    std::thread p([&] {
      item a(1, 1, 0);
      q.push_run(&a, 1);  // takes cell 0 and is held
    });
    lemma::stall_gate.wait_arrival();
    std::atomic<bool> first_done{false};
    item first;
    lemma::gate took;
    std::thread c1([&] {
      t_hold[1] = &took;
      EXPECT(q.try_pop(first));  // takes cell 0 and waits for it
      first_done.store(true, std::memory_order_release);
    });
    took.wait_arrival();  // c1 has taken cell 0
    took.release();
    q.push(item(0, 2, 0));  // takes cell 1 and fills it before cell 0 is filled
    item second;
    EXPECT(q.try_pop(second));  // takes cell 1: the later push's element
    EXPECT(second.producer == 2);
    EXPECT(!first_done.load());
    lemma::stall_gate.release();
    p.join();
    c1.join();
    // Filled second, taken first: in a sequential order consistent with
    // both pops, the held push comes first, so it is linearised when it
    // took its cell, not when it filled it.
    EXPECT(first.producer == 1);
  }
  EXPECT(lemma::live.load() == 0);
}

void pop_waits_for_late_push() {
  {
    queue q;
    q.push(item(0, 9, 0));  // cell 0
    lemma::gate checked, took;
    item got;
    bool c_got = false;
    std::thread c([&] {
      t_hold[0] = &checked;
      t_hold[1] = &took;
      c_got = q.try_pop(got);
    });
    checked.wait_arrival();  // c has seen the queue non-empty
    item v;
    EXPECT(q.try_pop(v) && v.producer == 9);  // takes cell 0; deq at 1
    checked.release();
    took.wait_arrival();  // c took cell 1, which no push has taken
    q.push(item(0, 1, 0));  // takes cell 1 and fills it
    EXPECT(!q.try_pop(v));  // every taken cell is claimed: empty, rightly
    took.release();
    c.join();
    // c returns the push that took its cell after c did: c is linearised
    // after that push, not at its own fetch-and-add, which came first.
    EXPECT(c_got && got.producer == 1);
  }
  EXPECT(lemma::live.load() == 0);
}

void batch_torn_by_a_skip() {
  {
    queue q;
    q.push(item(0, 9, 0));
    q.push(item(0, 9, 1));  // cells 0 and 1

    lemma::gate a0, a1, b0;
    std::atomic<bool> a_done{false};
    item a_out;
    bool a_got = false;
    std::thread ca([&] {
      t_hold[0] = &a0;
      t_hold[1] = &a1;
      a_got = q.try_pop(a_out);
      a_done.store(true);
    });
    a0.wait_arrival();  // ca has seen the queue non-empty
    bool b_got = true;
    std::thread cb([&] {
      t_hold[0] = &b0;
      item out;
      b_got = q.try_pop(out);
    });
    b0.wait_arrival();  // so has cb
    item drop;
    EXPECT(q.try_pop(drop) && drop.seq == 0);
    EXPECT(q.try_pop(drop) && drop.seq == 1);  // the queue is empty, deq at 2
    a0.release();
    a1.wait_arrival();  // ca took cell 2 and has not read it
    b0.release();
    cb.join();          // cb took cell 3, found no push had taken it, skipped it
    EXPECT(!b_got);     // and answered empty, rightly: nothing was pushed

    // A batch of three takes cells 2 to 4; cell 3 is skipped, so the batch
    // keeps cell 2, gives up the rest, and pushes them again. Its producer
    // is held while it takes the second element back.
    lemma::arm_stall(11, 2);
    std::thread p([&] {
      item run[3] = {item(10, 1, 0), item(11, 1, 1), item(12, 1, 2)};
      q.push_run(run, 3);
    });
    lemma::stall_gate.wait_arrival();
    q.push(item(0, 2, 0));  // another producer: cell 5
    lemma::stall_gate.release();
    p.join();
    a1.release();
    ca.join();
    EXPECT(a_got && a_out.producer == 1 && a_out.seq == 0);
    std::vector<item> rest;
    item v;
    while (q.try_pop(v)) rest.push_back(v);
    EXPECT(rest.size() == 3);
    if (rest.size() == 3) {
      // The batch's order holds; its parts are not contiguous.
      EXPECT(rest[0].producer == 2);
      EXPECT(rest[1].producer == 1 && rest[1].seq == 1);
      EXPECT(rest[2].producer == 1 && rest[2].seq == 2);
    }
  }
  EXPECT(lemma::live.load() == 0);
}

void segment_kept_for_reader() {
  {
    queue q;
    for (long i = 0; i < 8; ++i) q.push(item(0, 1, i));
    item v;
    for (long i = 0; i < 7; ++i) EXPECT(q.try_pop(v) && v.seq == i);
    lemma::gate h1;
    item last;
    bool got = false;
    std::thread c([&] {
      t_hold[1] = &h1;
      got = q.try_pop(last);  // takes cell 7, the segment's last, and is held
    });
    h1.wait_arrival();
    // Move the head on and retire many segments; the held pop protects the
    // first one, which must survive the scans.
    for (long i = 0; i < 400; ++i) q.push(item(0, 2, i));
    for (long i = 0; i < 400; ++i) EXPECT(q.try_pop(v) && v.producer == 2 && v.seq == i);
    h1.release();
    c.join();
    EXPECT(got && last.producer == 1 && last.seq == 7);
  }
  EXPECT(lemma::live.load() == 0);
}

void destroyed_with_elements_left() {
  {
    queue q;
    for (long i = 0; i < 30; ++i) q.push(item(0, 1, i));
    item v;
    for (long i = 0; i < 11; ++i) EXPECT(q.try_pop(v));
  }
  EXPECT(lemma::live.load() == 0);
}

template <bool kMany>
void stress(long per_producer) {
  const int producers = 4, consumers = kMany ? 4 : 1;
  {
    wtc::rt::segment_queue<item, kMany, 64, 64, hooks> q;
    lemma::empty_check check;
    std::atomic<int> producers_left{producers};
    std::vector<std::vector<long>> last(consumers, std::vector<long>(producers, -1));
    std::atomic<long> popped{0};
    std::vector<std::thread> ts;
    for (int p = 0; p < producers; ++p)
      ts.emplace_back([&, p] {
        std::mt19937 r(p + 1);
        lemma::move_jitter = 3u + p;
        long s = 0;
        while (s < per_producer) {
          if (r() % 7 == 0) {
            item run[5];
            long n = 0;
            for (; n < 5 && s < per_producer; ++n, ++s) run[n] = item(0, p, s);
            q.push_run(run, n);
            check.pushed(n);
          } else {
            q.push(item(0, p, s++));
            check.pushed();
          }
          if (r() % 97 == 0) std::this_thread::yield();
        }
        producers_left.fetch_sub(1);
      });
    for (int c = 0; c < consumers; ++c)
      ts.emplace_back([&, c] {
        t_random_hold = 7u + c;
        item v;
        for (;;) {
          bool got = check.pop([&] { return q.try_pop(v); });
          if (got) {
            EXPECT(v.seq > last[c][v.producer]);  // each producer's order, as this consumer sees it
            last[c][v.producer] = v.seq;
            popped.fetch_add(1);
          } else if (producers_left.load() == 0 && popped.load() == producers * per_producer) {
            break;
          }
        }
      });
    for (auto& t : ts) t.join();
    EXPECT(popped.load() == producers * per_producer);
    EXPECT(check.violations.load() == 0);
  }
  EXPECT(lemma::live.load() == 0);
}

}  // namespace

int main(int argc, char** argv) {
  const long n = argc > 1 ? std::atol(argv[1]) : 100000;
  held_push_is_waited_for();
  linearised_at_the_take();
  pop_waits_for_late_push();
  batch_torn_by_a_skip();
  segment_kept_for_reader();
  destroyed_with_elements_left();
  stress<true>(n);
  stress<false>(n);
  if (lemma::failures.load()) {
    std::fprintf(stderr, "lemma_segment_test: %d failure(s)\n", lemma::failures.load());
    return 1;
  }
  std::puts("lemma_segment_test: ok");
  return 0;
}
