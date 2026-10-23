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
// any number of consumers: the spsc ring, whose consumers take turns under
// a spin lock (runtime/spmc_ring.hpp).
enum class topology : int {
    generic = 0,
    spsc = 1,
    spsc_inline = 2,
    mpsc = 3,
    spmc = 4,
};

}
