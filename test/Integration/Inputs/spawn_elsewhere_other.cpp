// The other file of wtc_cc_spawn_elsewhere.cpp, built without wtc-cc: the
// thread entry that file starts, which pushes to the queue it is handed.
#include "queue.hpp"

void push_elsewhere(void* p) {
    auto* q = static_cast<wtc::queue<int>*>(p);
    for (int i = 0; i < 100000; ++i) q->push(i);
}
