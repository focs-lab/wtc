#pragma once

namespace wtc {

// Which implementation a queue object carries. The default is the
// general-purpose one, correct under any number of producers and consumers:
// a list of array segments whose cells each side takes with one
// fetch-and-add (runtime/segment_queue.hpp).
// The compiler, having proved a stronger fact about a particular object,
// rewrites the constant passed to that object's constructor; nothing else in
// the program changes. A tag the program states explicitly is left alone.
//
// The underlying type is int on purpose: ClangIR materialises a defaulted
// argument at the call site as `cir.const #cir.int<0> : !s32i`, and the
// specialisation pass matches exactly that.
//
// spsc_inline is spsc with the elements held inline in the ring, so that a
// push allocates nothing. It is only correct when every consumer of the
// object borrows what it pops: it keeps no copy of the returned shared_ptr
// and no pointer into the element past its next try_pop, because the
// element is destroyed in place then. The compiler proves that before it
// writes this tag. An element type that is not trivially destructible runs
// as spsc instead, since its destructor would otherwise run later than the
// program expects.
//
// mpsc is for any number of producers and one consumer: the same segmented
// queue with the consumer's side unshared, so that its pop takes no atomic
// read-modify-write and never skips a cell. spmc is for one producer and
// any number of consumers: segments the producer fills alone, whose cells
// the consumers claim with a compare-and-swap (runtime/spmc_queue.hpp).
enum class topology : int {
    generic = 0,
    spsc = 1,
    spsc_inline = 2,
    mpsc = 3,
    spmc = 4,
    // Chosen for a path's progress requirement, and only where the
    // runtime announces them: the queue's constructor calls a marker
    // annotated wtc_queue_impl_<name> with the tag (shim/queue.hpp,
    // shim/bounded.hpp), so the compiler writes this runtime's number and
    // never assumes one.
    //
    // scq and wcq are for a bounded queue: the rings of indices of
    // runtime/index_ring.hpp, lock-free and wait-free, any number of
    // producers and consumers up to a bound on threads, which their
    // markers carry as a second argument (shim/bounded.hpp). wcq is
    // announced only where the build has a double-width compare-and-swap
    // that is one instruction.
    scq = 5,
    wcq = 6,
    // jiffy is for an unbounded queue with one consumer: the mpsc form
    // with the push that takes its index from one counter for the whole
    // queue (runtime/segment_queue.hpp, kOneCounter), wait-free where
    // mpsc's push is lock-free with many producers. A tag of its own
    // because that push costs throughput, so a program gets it only where
    // a path requires the push to be wait-free.
    jiffy = 7,
    // The settled bounded rings: the bounded ring whose single side publishes
    // its counter only after it has filled or emptied its cell
    // (runtime/bounded_ring.hpp, built settled), so that with one producer no
    // pop waits for a push and with one consumer no push waits for a pop.
    // Bounded only; tags of their own because the order costs throughput, so a
    // program gets them only where a path requires it.
    spsc_settled = 8,
    mpsc_settled = 9,
    spmc_settled = 10,
};

}
