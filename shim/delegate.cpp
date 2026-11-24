// The runtime behind --wtc-delegate-sections (docs/delegation.md): the entry
// point a delegated critical section is called through.
// tools/wtc-cc compiles this file with the program's WTC_* definitions and
// links it into every program in which a section was delegated.
//
//   wtc_delegate_run(key, fn, env)
//
// runs fn(env), a critical section outlined with its mutex's own lock and
// unlock, on the calling thread while the mutex at `key` is free or
// lightly contended, or when the calling thread is a server itself, and on
// the mutex's server while it is contended (runtime/delegate.hpp); returns
// fn's result, the exit by which the section left, once fn has returned.

#include "../runtime/delegate.hpp"

extern "C" int wtc_delegate_run(void* key, int (*fn)(void*), void* env) {
    return wtc::rt::delegation::run(key, fn, env);
}
