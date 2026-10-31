// A reduced model of g3log 2.6.2's queue, read as written: shared_queue
// (src/g3log/shared_queue.hpp, verbatim but for the comments) inside the
// Active object (src/g3log/active.hpp) that runs a log worker. Many logging
// threads send closures to one worker thread, which runs them; the worker
// stops at a closure that sets its done flag, sent by the destructor, so
// the queue has no stop flag of its own. Nothing is written for the
// compiler.
//
// The class lifts: push, try_and_pop and the blocking wait_and_pop match,
// and the condition variable is notified after every push. wait_and_pop
// becomes wtc::queue's pop_wait, which sleeps as the original did. The
// worker is the one consumer and the loggers are many producers, so the
// queue is the single-consumer one. Every message arrives once, under
// ThreadSanitizer and with WTC_CHECK.
//
// The code kept is the upstream code, reduced; g3log is in the public domain (Unlicense).
//
// RUN: %wtc_cc -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe | FileCheck %s --check-prefix=OUT
// RUN: %if tsan %{ %wtc_cc --tsan -O1 -g %s -o %t.tsan.exe 2>&1 | FileCheck %s --check-prefix=CHECK %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 2>&1 | FileCheck %s --check-prefixes=TSAN,OUT %}
//
// CHECK-NOT: locked queue {{.*}} not lifted: (
// CHECK: locked queue queue_ in shared_queue<std::function<void ()>> lifted (shared_queue<std::function<void ()>>::push: push, shared_queue<std::function<void ()>>::wait_and_pop: blocking pop)
// CHECK: queue queue_ in any shared_queue<std::function<void ()>>: mpsc (producers: many, consumers: 1)
// CHECK: specialised: constructor tag rewritten to mpsc
// TSAN-NOT: ThreadSanitizer
// OUT: messages 40000 done 1

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

template <typename T>
class shared_queue {
   std::queue<T> queue_;
   mutable std::mutex m_;
   std::condition_variable data_cond_;

   shared_queue& operator=(const shared_queue&) = delete;
   shared_queue(const shared_queue& other) = delete;

  public:
   shared_queue() = default;

   void push(T item) {
      {
         std::lock_guard<std::mutex> lock(m_);
         queue_.push(std::move(item));
      }
      data_cond_.notify_one();
   }

   bool try_and_pop(T& popped_item) {
      std::lock_guard<std::mutex> lock(m_);
      if (queue_.empty()) {
         return false;
      }
      popped_item = std::move(queue_.front());
      queue_.pop();
      return true;
   }

   void wait_and_pop(T& popped_item) {
      std::unique_lock<std::mutex> lock(m_);
      while (queue_.empty()) {
         data_cond_.wait(lock);
      }
      popped_item = std::move(queue_.front());
      queue_.pop();
   }

   bool empty() const {
      std::lock_guard<std::mutex> lock(m_);
      return queue_.empty();
   }

   unsigned size() const {
      std::lock_guard<std::mutex> lock(m_);
      return queue_.size();
   }
};

namespace kjellkod {
typedef std::function<void()> Callback;

class Active {
  private:
   Active() : done_(false) {}
   Active(const Active&) = delete;
   Active& operator=(const Active&) = delete;

   void run() {
      while (!done_) {
         Callback func;
         mq_.wait_and_pop(func);
         func();
      }
   }

   shared_queue<Callback> mq_;
   std::thread thd_;
   bool done_;

  public:
   virtual ~Active() {
      send([this]() noexcept { done_ = true; });
      thd_.join();
   }

   void send(Callback msg_) { mq_.push(msg_); }

   static std::unique_ptr<Active> createActive() {
      std::unique_ptr<Active> aPtr(new Active());
      aPtr->thd_ = std::thread(&Active::run, aPtr.get());
      return aPtr;
   }
};
}  // namespace kjellkod

// The log worker's sink: one per worker, written only by the worker thread.
long written = 0;

int main() {
   constexpr int kLoggers = 4, kPerLogger = 10000;
   auto worker = kjellkod::Active::createActive();
   std::vector<std::thread> loggers;
   for (int i = 0; i < kLoggers; ++i)
      loggers.emplace_back([&] {
         for (int k = 0; k < kPerLogger; ++k) worker->send([] { ++written; });
      });
   for (auto& t : loggers) t.join();
   worker.reset();  // sends the done message and joins the worker
   std::printf("messages %ld done %d\n", written, 1);
}
