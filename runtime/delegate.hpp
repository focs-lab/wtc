// wtc::rt::delegation: the critical sections of a mutex run by one server
// thread, which the clients hand them to over the single-consumer segmented
// queue (segment_queue.hpp, kManyConsumers = false), each client waiting
// for its own section with a waiter (waiter.hpp).
//
// What the compiler hands it (--wtc-delegate-sections, docs/delegation.md):
// a critical section of one mutex outlined into a function, `int fn(void
// *env)`, whose body is the section as written, the mutex's own lock at its
// start and its own unlock at its end included; the environment holds the
// addresses of the values the section reads and of the slots it writes for
// the code after it; the result says by which of its exits the section
// left. Running fn on the client's own thread is the original program.
// run(key, fn, env) runs it instead on the server of `key`, the mutex's
// address, and returns its result once it has returned.
//
// The facts the compiler proves before it hands a section here, and what
// breaks if one is false (docs/delegation.md, "The contract"):
//
//   - the section does not depend on which thread runs it: no thread-local
//     storage, errno, thread id, signal state, thread-owned lock taken
//     without being let go inside it; otherwise the server's is what it
//     sees, not the client's;
//   - it does not wait for another thread but by taking a lock it lets go
//     again inside it, and takes no recursive mutex; otherwise the server
//     stalls every client of the mutex, or waits for one of them;
//   - it does not throw out of itself: an exception would unwind the
//     server, not the client.
//
// Why one server per mutex. The server stands for the mutex: a client
// waits for it as it waited for the mutex, and while the server runs a
// client's section that client is, as far as every other thread can tell,
// inside its section. So a run of the delegating program in which threads
// wait for each other in a cycle is a run of the original in which they
// waited for the same locks, provided the server serialises the sections
// of exactly one mutex. A server shared by two mutexes would order their
// sections, which the original did not: a thread holding one of them while
// it delegates a section of the other, which a delegated section of the
// first is waiting to run behind, is a deadlock the original did not have
// (memcached's LRU maintainer, in the 23 Nov probe). The server of a mutex
// is therefore keyed by the mutex's address, and started the first time a
// section of it is delegated.
//
// The server takes the mutex itself, inside fn, as the original section
// does; a thread that runs a section inline (below), or a section of the
// same mutex the compiler did not delegate, takes it as before, and
// exclusion is the original's whichever way a section runs.
//
// Who runs inline. A server that reaches a delegated section, of its own
// mutex or of another, runs it on its own thread: the server must never
// wait for itself, and on another server it would wait while holding its
// mutex, which the original did on that thread too. Everything else is a
// client. Order: the sections of one client run in its program order (it
// waits for each); between clients they run in the order the queue
// linearises their pushes, which is an order the original's mutex could
// have granted.
//
// Happens-before. A client's push happens before the server's pop of it
// (release, acquire), so everything the client did before the section is
// visible to it; the server's store of `done` (release) and the client's
// load of it (acquire) make everything the section did visible to the
// client after it, as the original's unlock did. Between sections of
// different clients the mutex itself, taken by fn, orders them as before.
//
// Waiting. A client polls its request's flag through its own waiter: spin
// for WTC_DELEGATE_SPIN_NS (50 microseconds unless set), then park. The
// server, having set the flag, wakes the client only if it reads the client
// announced as asleep, without the waiter's fence: on the server the fence
// would cost a line transfer per section, waiting for the flag's store to
// reach the client spinning on it, where an unfenced store leaves it to the
// store buffer while the server goes on to the next request. Read without
// the fence, the announcement may be missed by a client just going to
// sleep, so a client's park is bounded (kParkLimit, one millisecond) and it
// then looks again: a missed wake-up costs that much time, never progress.
// An idle server spins as long and then parks with no time limit, and
// costs nothing while it sleeps; a client wakes it with the waiter's own
// fenced notify after its push, on the client's side, where the push is a
// read-modify-write already. The waiters are allocated once per thread and
// never freed, so the server's wake-up after a client has already seen the
// flag and left touches live memory, at worst a waiter a later thread has
// taken over, which a spurious wake-up only makes look again.
//
// The servers' lives. A thread becomes a client at its first delegated
// section and stops being one when it ends (its thread-local lease); a
// section it reaches after that, from another thread-local object's
// destructor, runs on it. When the last client ends, every server is woken
// and ends too, so that a program whose main thread ends with pthread_exit
// ends when its last own thread does, as written; a later client starts
// them again. At most WTC_DELEGATE_MAX_SERVERS (4 unless set) run at once:
// the sections of a mutex delegated when as many are running run on their
// client, under the mutex, as written, and so do they when a server cannot
// be started (pthread_create failing for want of memory or threads). That
// bounds the threads a class with one mutex per object can start; a thread
// that runs a section as written is always faithful, it only gains nothing.
//
// What a server is started with. Every signal blocked: an asynchronous
// signal must go to a thread of the program's, never to the server, whose
// mask would otherwise be that of the client that happened to start it. A
// stack as large as that client's (at least the default, at most 1 GiB):
// a section never has less stack on the server than it had on that
// client, and a client whose own stack is larger than its server's runs
// its sections itself. The affinity and scheduling of the client that
// starts it, unless WTC_DELEGATE_CPU pins it. A process that forks gets no
// server in the child: the child's runtime starts empty (pthread_atfork),
// and its first delegated section starts a server of its own.
//
// Environment, read once at the first server's start:
//
//   WTC_DELEGATE_CPU=LIST         pin the servers, in the order they start,
//                                 to these CPUs (a comma-separated list,
//                                 used round robin); unset, a server takes
//                                 its starting client's affinity
//   WTC_DELEGATE_WAIT=spin        the servers never park when idle
//   WTC_DELEGATE_SPIN_NS=N        spin this long before parking (clients,
//                                 servers)
//   WTC_DELEGATE_MAX_SERVERS=N    at most N servers at once (default 4)
//   WTC_DELEGATE_REPORT=1         print each server's count of sections at
//                                 exit
//
// A thread started inside a delegated section inherits the server's
// signal mask (every signal blocked), affinity and scheduling: what its
// starting client had when it started the server (docs/delegation.md,
// "Assumptions", assume-uniform-threads).
//
// Measured: not yet in a leg. The 23 Nov probe (runs outside the legs,
// not committed), LevelDB readrandom with the hand-written equivalent of
// this runtime and a server that always spins: 8 threads, 1.77x the
// original and 1.45x the best LiTL lock; 4 threads, 0.95x the best LiTL
// lock. The server takes a core while it has work.

#ifndef WTC_RT_DELEGATE_HPP
#define WTC_RT_DELEGATE_HPP

#include <pthread.h>
#include <sched.h>
#include <signal.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

#include "segment_queue.hpp"
#include "waiter.hpp"

namespace wtc::rt {

class delegation {
public:
    using section = int (*)(void*);

    // fn(env) on the server of `key`, or on this thread when it is a
    // server, is ending, has more stack than the server, or no server can
    // be had; its result either way.
    static int run(const void* key, section fn, void* env) {
        thread_state& me = state();
        if (me.on_server || me.exiting || !become_client(me)) return fn(env);
        server* s = nullptr;
        if (me.last_key == key && me.gen == generation().load(std::memory_order_acquire)) {
            if (me.last_inline) return fn(env);
            s = me.last;
        } else {
            s = find(key, me);
        }
        if (!s || me.stack > s->stack) return fn(env);
        request r;
        r.fn = fn;
        r.env = env;
        r.client = me.client;
        s->queue.push(&r);
        s->idle.notify(1);
        auto done = [&] { return r.done.load(std::memory_order_acquire); };
        while (!me.client->wait_for(done, kParkLimit)) {
        }
        return r.result;
    }

    // The servers started so far in this process, ended ones included, and
    // the sections each has run: for tests and the report.
    static std::vector<std::uint64_t> served() {
        registry& g = reg();
        std::lock_guard<std::mutex> lock(g.m);
        std::vector<std::uint64_t> out;
        for (server* s = g.first; s; s = s->next) out.push_back(s->served.load(std::memory_order_relaxed));
        return out;
    }

    // Servers running now.
    static unsigned running() {
        registry& g = reg();
        std::lock_guard<std::mutex> lock(g.m);
        return g.active;
    }

    // Whether the calling thread is a server.
    static bool on_server() { return state().on_server; }

private:
    // How long a client sleeps at most before it looks again (see
    // "Waiting" above).
    static constexpr std::chrono::nanoseconds kParkLimit{1000000};
    static constexpr std::size_t kMaxStack = std::size_t{1} << 30;

    struct request {
        section fn = nullptr;
        void* env = nullptr;
        waiter* client = nullptr;
        int result = 0;
        std::atomic<bool> done{false};
    };

    struct server {
        server(const void* k, wait_policy p, std::uint64_t spin) : key(k), idle(p, spin) {}
        const void* key;
        server* next = nullptr;
        segment_queue<request*, /*kManyConsumers=*/false> queue;
        alignas(64) waiter idle;
        alignas(64) std::atomic<std::uint64_t> served{0};
        std::atomic<bool> stop{false};
        bool ended = false;  // under the registry's lock
        std::size_t stack = 0;
        pthread_t thread{};
    };

    struct registry {
        std::mutex m;
        server* first = nullptr;  // every server started, ended ones included
        unsigned started = 0;
        unsigned active = 0;      // servers running
        unsigned live = 0;        // client threads that have not ended
        bool read_env = false;
        bool spin = false;
        bool report = false;
        unsigned max_servers = 4;
        std::vector<int> cpus;
    };

    // Trivially destructible: readable from any thread-local destructor.
    struct thread_state {
        bool on_server = false;
        bool exiting = false;
        bool last_inline = false;
        unsigned gen = 0;          // the registry generation the fields below belong to
        unsigned counted_gen = ~0u;  // the generation whose live count includes this thread
        const void* last_key = nullptr;
        server* last = nullptr;
        waiter* client = nullptr;
        std::size_t stack = 0;
    };

    static thread_state& state() {
        static thread_local thread_state me;
        return me;
    }

    // A fork's child starts with an empty runtime: a new registry and pool
    // (the old ones, whose locks another thread may have held, are left).
    static std::atomic<unsigned>& generation() {
        static std::atomic<unsigned> g{0};
        return g;
    }
    static std::atomic<registry*>& reg_ptr() {
        static std::atomic<registry*> p{new registry};
        return p;
    }
    static registry& reg() { return *reg_ptr().load(std::memory_order_acquire); }

    struct waiter_pool {
        std::mutex m;
        std::vector<waiter*> free;
    };
    static std::atomic<waiter_pool*>& pool_ptr() {
        static std::atomic<waiter_pool*> p{new waiter_pool};
        return p;
    }

    static void after_fork_in_child() {
        reg_ptr().store(new registry, std::memory_order_release);
        pool_ptr().store(new waiter_pool, std::memory_order_release);
        generation().fetch_add(1, std::memory_order_acq_rel);
    }

    // A client's waiter, from a pool that is never freed: a thread that
    // ends gives its waiter back, and a later thread takes it over. The
    // lease's destructor is the client's end.
    struct waiter_lease {
        waiter* w = nullptr;
        ~waiter_lease() {
            thread_state& me = state();
            me.exiting = true;
            if (me.counted_gen == generation().load(std::memory_order_acquire)) client_ended();
            me.client = nullptr;
            if (!w) return;
            waiter_pool& p = *pool_ptr().load(std::memory_order_acquire);
            std::lock_guard<std::mutex> lock(p.m);
            p.free.push_back(w);
        }
    };

    // Makes the calling thread a client of this generation's registry:
    // a waiter, its stack size, and its count among the live clients.
    static bool become_client(thread_state& me) {
        static thread_local waiter_lease lease;
        const unsigned gen = generation().load(std::memory_order_acquire);
        if (me.counted_gen == gen) return true;
        if (!me.client) {
            waiter_pool& p = *pool_ptr().load(std::memory_order_acquire);
            {
                std::lock_guard<std::mutex> lock(p.m);
                if (!p.free.empty()) {
                    lease.w = p.free.back();
                    p.free.pop_back();
                }
            }
            if (!lease.w) lease.w = new (std::nothrow) waiter(wait_policy::spin_then_park, spin_ns());
            if (!lease.w) return false;
            me.client = lease.w;
            pthread_attr_t a;
            if (pthread_getattr_np(pthread_self(), &a) == 0) {
                std::size_t size = 0;
                pthread_attr_getstacksize(&a, &size);
                pthread_attr_destroy(&a);
                me.stack = size;
            }
        }
        registry& g = reg();
        std::lock_guard<std::mutex> lock(g.m);
        ++g.live;
        me.counted_gen = gen;
        me.gen = gen;
        me.last_key = nullptr;
        me.last = nullptr;
        return true;
    }

    // The last client has ended: every server is told to end.
    static void client_ended() {
        registry& g = reg();
        std::lock_guard<std::mutex> lock(g.m);
        if (g.live == 0 || --g.live != 0) return;
        for (server* s = g.first; s; s = s->next)
            if (!s->ended) {
                s->stop.store(true, std::memory_order_release);
                s->idle.notify(1);
            }
    }

    static std::uint64_t spin_ns() {
        static const std::uint64_t ns = [] {
            const char* e = std::getenv("WTC_DELEGATE_SPIN_NS");
            return e && *e ? std::strtoull(e, nullptr, 10) : std::uint64_t{50000};
        }();
        return ns;
    }

    static void read_env(registry& g) {
        if (g.read_env) return;
        g.read_env = true;
        if (const char* w = std::getenv("WTC_DELEGATE_WAIT")) g.spin = std::strcmp(w, "spin") == 0;
        if (const char* r = std::getenv("WTC_DELEGATE_REPORT")) g.report = *r && std::strcmp(r, "0") != 0;
        if (const char* m = std::getenv("WTC_DELEGATE_MAX_SERVERS")) g.max_servers = static_cast<unsigned>(std::strtoul(m, nullptr, 10));
        if (const char* c = std::getenv("WTC_DELEGATE_CPU")) {
            const char* p = c;
            while (*p) {
                char* end = nullptr;
                long v = std::strtol(p, &end, 10);
                if (end == p) break;
                if (v >= 0 && v < CPU_SETSIZE) g.cpus.push_back(static_cast<int>(v));
                p = *end == ',' ? end + 1 : end;
            }
        }
        static std::once_flag once;
        std::call_once(once, [&] {
            pthread_atfork(nullptr, nullptr, after_fork_in_child);
            if (g.report) std::atexit(report);
        });
    }

    static void report() {
        registry& g = reg();
        std::lock_guard<std::mutex> lock(g.m);
        unsigned i = 0;
        for (server* s = g.first; s; s = s->next, ++i)
            std::fprintf(stderr, "wtc-delegate: server %u ran %llu sections\n", i,
                         static_cast<unsigned long long>(s->served.load(std::memory_order_relaxed)));
    }

    // The running server of `key`, started if there is none and room for
    // one; null when its sections run on their clients.
    static server* find(const void* key, thread_state& me) {
        registry& g = reg();
        server* s = nullptr;
        {
            std::lock_guard<std::mutex> lock(g.m);
            read_env(g);
            for (server* t = g.first; t; t = t->next)
                if (t->key == key && !t->ended) s = t;
            if (!s && g.active < g.max_servers) s = start(g, key, me.stack);
        }
        me.last_key = key;
        me.last = s;
        me.last_inline = !s;
        return s;
    }

    // With the registry's lock held.
    static server* start(registry& g, const void* key, std::size_t client_stack) {
        server* s = new (std::nothrow) server(key, g.spin ? wait_policy::spin : wait_policy::spin_then_park, spin_ns());
        if (!s) return nullptr;
        pthread_attr_t a;
        pthread_attr_init(&a);
        std::size_t size = 0;
        pthread_attr_getstacksize(&a, &size);
        if (client_stack > size) size = client_stack < kMaxStack ? client_stack : kMaxStack;
        if (pthread_attr_setstacksize(&a, size) == 0) s->stack = size;
        else pthread_attr_getstacksize(&a, &s->stack);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        // Every signal blocked from its first instruction: inherited from
        // this thread's mask, set for the call.
        sigset_t all, old;
        sigfillset(&all);
        pthread_sigmask(SIG_SETMASK, &all, &old);
        const int e = pthread_create(&s->thread, &a, serve, s);
        pthread_sigmask(SIG_SETMASK, &old, nullptr);
        pthread_attr_destroy(&a);
        if (e) {
            delete s;
            return nullptr;
        }
        if (!g.cpus.empty()) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(g.cpus[g.started % g.cpus.size()], &set);
            if (pthread_setaffinity_np(s->thread, sizeof set, &set))
                std::fprintf(stderr, "wtc-delegate: cannot pin a server to CPU %d\n",
                             g.cpus[g.started % g.cpus.size()]);
        }
        ++g.started;
        ++g.active;
        // Last in the list: served() lists the servers in the order they
        // started.
        server** at = &g.first;
        while (*at) at = &(*at)->next;
        *at = s;
        return s;
    }

    static void* serve(void* arg) noexcept {
        server* s = static_cast<server*>(arg);
        registry& g = reg();
        state().on_server = true;
        request* r = nullptr;
        std::uint64_t served = 0;
        auto pop = [&] { return s->queue.try_pop(r); };
        for (;;) {
            if (!pop()) {
                s->idle.wait([&] { return pop() || s->stop.load(std::memory_order_acquire); });
                if (!r) {
                    // Told to end: end if no client is left, else go on.
                    std::lock_guard<std::mutex> lock(g.m);
                    s->stop.store(false, std::memory_order_relaxed);
                    if (g.live == 0 && !pop()) {
                        s->ended = true;
                        --g.active;
                        return nullptr;
                    }
                    if (!r) continue;
                }
            }
            const int result = r->fn(r->env);
            waiter* client = r->client;
            r->result = result;
            // After this store the request, on the client's stack, may be
            // gone; the client's waiter is not (waiter_pool).
            r->done.store(true, std::memory_order_release);
            if (client->waiting()) client->notify(1);
            s->served.store(++served, std::memory_order_relaxed);
            r = nullptr;
        }
        return nullptr;
    }
};

}  // namespace wtc::rt

#endif
