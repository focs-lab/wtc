// A reduced model of Bitcoin Core's CCheckQueue (src/checkqueue.h,
// 9bf4815), read as written but for its synchronisation wrappers: Bitcoin's
// Mutex is std::mutex with thread-safety annotations and its WAIT_LOCK and
// LOCK are std::unique_lock, so the model uses those. One master thread adds
// batches of checks and then joins the workers in Complete() as one more
// consumer until every check is done; the result is the first failure.
//
// The class does not lift, and the remarks say why: the container is a
// std::vector used as a stack (checks are taken from the back), batches go
// in and out by range insert and erase, the batch size is computed from
// size(), and the mutex also guards the counts of idle, total and
// unfinished checks and the result. The program runs as written.
//
// The code kept is the upstream code, reduced; Bitcoin Core is under the MIT licence.
//
// RUN: %wtc_cc -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe | FileCheck %s --check-prefix=OUT
//
// CHECK-DAG: locked queue queue in CCheckQueue<Check, bool> not lifted: (e) the container is a std::vector, which the class may use as a stack or a batch
// CHECK-DAG: locked queue queue in CCheckQueue<Check, bool> not lifted: (c) the mutex m_mutex also guards nTodo in CCheckQueue<Check, bool>::Add
// CHECK-DAG: locked queue queue in CCheckQueue<Check, bool> not lifted: (e) CCheckQueue<Check, bool>::Add inserts or removes in the middle (insert)
// CHECK-NOT: lifted (
// OUT: checks 10000 result none

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

template <typename T, typename R>
class CCheckQueue {
private:
    std::mutex m_mutex;
    std::condition_variable m_worker_cv;
    std::condition_variable m_master_cv;
    std::vector<T> queue;
    int nIdle{0};
    int nTotal{0};
    std::optional<R> m_result;
    unsigned int nTodo{0};
    const unsigned int nBatchSize;
    std::vector<std::thread> m_worker_threads;
    bool m_request_stop{false};

    std::optional<R> Loop(bool fMaster) {
        std::condition_variable& cond = fMaster ? m_master_cv : m_worker_cv;
        std::vector<T> vChecks;
        vChecks.reserve(nBatchSize);
        unsigned int nNow = 0;
        std::optional<R> local_result;
        bool do_work;
        do {
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                if (nNow) {
                    if (local_result.has_value() && !m_result.has_value()) {
                        std::swap(local_result, m_result);
                    }
                    nTodo -= nNow;
                    if (nTodo == 0 && !fMaster) {
                        m_master_cv.notify_one();
                    }
                } else {
                    nTotal++;
                }
                while (queue.empty() && !m_request_stop) {
                    if (fMaster && nTodo == 0) {
                        nTotal--;
                        std::optional<R> to_return = std::move(m_result);
                        m_result = std::nullopt;
                        return to_return;
                    }
                    nIdle++;
                    cond.wait(lock);
                    nIdle--;
                }
                if (m_request_stop) {
                    return std::nullopt;
                }
                nNow = std::max(1U, std::min(nBatchSize, (unsigned int)queue.size() / (nTotal + nIdle + 1)));
                auto start_it = queue.end() - nNow;
                vChecks.assign(std::make_move_iterator(start_it), std::make_move_iterator(queue.end()));
                queue.erase(start_it, queue.end());
                do_work = !m_result.has_value();
            }
            if (do_work) {
                for (T& check : vChecks) {
                    local_result = check();
                    if (local_result.has_value()) break;
                }
            }
            vChecks.clear();
        } while (true);
    }

public:
    explicit CCheckQueue(unsigned int batch_size, int worker_threads_num) : nBatchSize(batch_size) {
        m_worker_threads.reserve(worker_threads_num);
        for (int n = 0; n < worker_threads_num; ++n)
            m_worker_threads.emplace_back([this]() { Loop(false); });
    }

    std::optional<R> Complete() { return Loop(true); }

    void Add(std::vector<T>&& vChecks) {
        if (vChecks.empty()) {
            return;
        }
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            queue.insert(queue.end(), std::make_move_iterator(vChecks.begin()), std::make_move_iterator(vChecks.end()));
            nTodo += vChecks.size();
        }
        if (vChecks.size() == 1) {
            m_worker_cv.notify_one();
        } else {
            m_worker_cv.notify_all();
        }
    }

    ~CCheckQueue() {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_request_stop = true;
        }
        m_worker_cv.notify_all();
        for (std::thread& t : m_worker_threads) {
            t.join();
        }
    }
};

// A script check: empty, as in the project's own benchmark of the queue.
struct Check {
    int n = 0;
    std::optional<bool> operator()() const { return std::nullopt; }
};

int main() {
    CCheckQueue<Check, bool> q(128, 3);
    int added = 0;
    for (int b = 0; b < 100; ++b) {
        std::vector<Check> batch(100);
        added += 100;
        q.Add(std::move(batch));
    }
    std::optional<bool> r = q.Complete();
    std::printf("checks %d result %s\n", added, r ? "failure" : "none");
}
