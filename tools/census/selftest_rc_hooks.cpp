// Reference counts with a known census, through rc_hooks.h, which sees
// releases only: shared_ptr a is copied and released 1000 times by the
// main thread, b 250 times by each of four threads, and each is released
// once more when it dies, in main. The report must say one single-thread
// block of two and 1001 of 2002 releases on it.
#include "rc_hooks.h"

#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

static void churn(const std::shared_ptr<int>& p, int n) {
    for (int i = 0; i < n; ++i) {
        std::shared_ptr<int> q = p;  // a copy, released at the end of the iteration
        (void)q;
    }
}

int main() {
    auto a = std::make_shared<int>(1);
    auto b = std::make_shared<int>(2);
    std::vector<std::thread> ts;
    for (int i = 0; i < 3; ++i) ts.emplace_back(churn, std::cref(b), 250);
    churn(b, 250);
    for (auto& t : ts) t.join();
    churn(a, 1000);
    std::puts("done");
    return 0;
}
