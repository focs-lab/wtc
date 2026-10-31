// The other file of other_unit.cpp, built without wtc-cc.
#include "other_unit.h"
std::size_t report(LQ<int>& q) { return q.pending(); }
void produce(LQ<int>& q, int n) {
    for (int i = 0; i < n; ++i) q.push(100 + i);
}
