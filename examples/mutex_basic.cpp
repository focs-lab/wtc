#include "mutex.hpp"

void f(wtc::mutex &m) {
    m.lock();
    m.unlock();
}

int main() {
    wtc::mutex m;
    f(m);
    return 0;
}