// A reduced model of BS::thread_pool (include/BS_thread_pool.hpp, built as
// C++17: no jthread, no priorities, no pausing), read as written: the task
// queue is a std::queue<std::function<void()>> under tasks_mutex, workers
// wait on task_available_cv with the predicate "tasks queued or the pool
// stopping", and wait() sleeps on tasks_done_cv until no task is queued or
// running. The worker loop, detach_task, wait, the task-count getters and
// destroy_threads keep their source's shape.
//
// The class does not lift, and the remarks say why: the mutex also guards
// the running count, the waiting flag and the stop flag outside any queue
// operation; a worker pops through pop_task(), which touches the queue
// without taking the mutex itself; wait() decides on tasks.empty()
// together with the running count; get_tasks_queued() reads size(); and
// the worker returns at the stop flag with tasks still queued. Each of
// these is a joint invariant of the queue and the pool that a lock-free
// queue alone cannot keep. The program runs as written.
//
// The code kept is the upstream code, reduced; BS::thread_pool is under the MIT licence.
//
// RUN: %wtc_cc -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe | FileCheck %s --check-prefix=OUT
//
// CHECK-DAG: locked queue tasks in BS::thread_pool not lifted: (c) the mutex tasks_mutex also guards tasks_running in BS::thread_pool::worker
// CHECK-DAG: locked queue tasks in BS::thread_pool not lifted: (b) BS::thread_pool::pop_task calls front on tasks without the mutex held
// CHECK-DAG: locked queue tasks in BS::thread_pool not lifted: (d) BS::thread_pool::get_tasks_queued reads size(): the substitutes keep no linearisable count
// CHECK-NOT: lifted (
// OUT: tasks 3000 queued 0

#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <atomic>

namespace BS {
class thread_pool {
public:
    using task_t = std::function<void()>;

    explicit thread_pool(std::size_t num_threads) : thread_count(num_threads), threads(new std::thread[num_threads]) {
        create_threads();
    }
    ~thread_pool() {
        wait();
        destroy_threads();
    }

    template <typename F>
    void detach_task(F&& task) {
        {
            const std::scoped_lock tasks_lock(tasks_mutex);
            tasks.emplace(std::forward<F>(task));
        }
        task_available_cv.notify_one();
    }

    [[nodiscard]] std::size_t get_tasks_queued() const {
        const std::scoped_lock tasks_lock(tasks_mutex);
        return tasks.size();
    }

    void wait() {
        std::unique_lock tasks_lock(tasks_mutex);
        waiting = true;
        tasks_done_cv.wait(tasks_lock, [this] { return (tasks_running == 0) && tasks.empty(); });
        waiting = false;
    }

private:
    void create_threads() {
        {
            const std::scoped_lock tasks_lock(tasks_mutex);
            tasks_running = thread_count;
            workers_running = true;
        }
        for (std::size_t i = 0; i < thread_count; ++i)
            threads[i] = std::thread([this, i] { worker(i); });
    }

    void destroy_threads() {
        {
            const std::scoped_lock tasks_lock(tasks_mutex);
            workers_running = false;
        }
        task_available_cv.notify_all();
        for (std::size_t i = 0; i < thread_count; ++i)
            threads[i].join();
    }

    task_t pop_task() {
        task_t task = std::move(tasks.front());
        tasks.pop();
        return task;
    }

    void worker(const std::size_t) {
        while (true) {
            std::unique_lock tasks_lock(tasks_mutex);
            --tasks_running;
            if (waiting && (tasks_running == 0) && tasks.empty())
                tasks_done_cv.notify_all();
            task_available_cv.wait(tasks_lock, [this] { return !tasks.empty() || !workers_running; });
            if (!workers_running)
                break;
            {
                task_t task = pop_task();
                ++tasks_running;
                tasks_lock.unlock();
                task();
            }
        }
    }

    mutable std::mutex tasks_mutex;
    std::condition_variable task_available_cv;
    std::condition_variable tasks_done_cv;
    std::queue<task_t> tasks;
    std::size_t tasks_running = 0;
    std::size_t thread_count = 0;
    std::unique_ptr<std::thread[]> threads;
    bool waiting = false;
    bool workers_running = false;
};
}  // namespace BS

std::atomic<long> done{0};

int main() {
    std::size_t queued;
    {
        BS::thread_pool pool(3);
        for (int i = 0; i < 3000; ++i) pool.detach_task([] { ++done; });
        pool.wait();
        queued = pool.get_tasks_queued();
    }
    std::printf("tasks %ld queued %zu\n", done.load(), queued);
}
