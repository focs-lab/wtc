// A queue class and a pool shaped as CTPL's (ctpl_stl.h): the queue under a
// mutex of its own; the pool's workers wait on the pool's condition
// variable, under the pool's mutex, for a task or the stop flag. Each test
// defines POOL_VARIANT_* to change one thing and checks what the pass says.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

template <typename T> class Queue {
public:
    bool push(T const &v) {
        std::unique_lock<std::mutex> lock(this->m);
        this->q.push(v);
        return true;
    }
    bool pop(T &v) {
        std::unique_lock<std::mutex> lock(this->m);
        if (this->q.empty()) return false;
        v = this->q.front();
        this->q.pop();
        return true;
    }

private:
    std::queue<T> q;
    std::mutex m;
};
