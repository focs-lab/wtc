#pragma once

namespace wtc {

// Which implementation a queue object carries. The default is the
// general-purpose one, correct under any number of producers and consumers.
// The compiler, having proved a stronger fact about a particular object,
// rewrites the constant passed to that object's constructor; nothing else in
// the program changes. A tag the program states explicitly is left alone.
//
// The underlying type is int on purpose: ClangIR materialises a defaulted
// argument at the call site as `cir.const #cir.int<0> : !s32i`, and the
// specialisation pass matches exactly that.
enum class topology : int {
    generic = 0,
    spsc = 1,
};

}
