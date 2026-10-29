// Another file of the program of wtc_cc_other_files.cpp, built without
// wtc-cc: it names that file's queue and pushes to it.
#include "queue.hpp"

extern wtc::queue<int> q;

void push_from_elsewhere(int n) {
    for (int i = 0; i < n; ++i) q.push(i);
}
