// wtc::rt::delegation: the critical sections of a mutex run by the thread
// that reaches them while the mutex is free or lightly contended, and by
// one server thread for the mutex while it is contended, which the clients
// hand them to over the single-consumer segmented queue (segment_queue.hpp,
// kManyConsumers = false), each client waiting for its own section with a
// waiter (waiter.hpp).
//
// What the compiler hands it (--wtc-delegate-sections, docs/delegation.md):
// a critical section of one mutex outlined into a function, `int fn(void
// *env)`, whose body is the section as written, the mutex's own lock at its
// start and its own unlock at its end included; the environment holds the
// addresses of the values the section reads and of the slots it writes for
// the code after it; the result says by which of its exits the section
// left. Running fn on the client's own thread is the original program.
// run(key, fn, env), key the mutex's address, runs it on this thread or on
// the server of `key`, and returns its result once it has returned.
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
// Where a section runs (docs/delegation.md, "Where a section runs"). A
// round trip to a server costs more than a free or lightly contended mutex
// (the leg of 24 Nov 2026: half the original's throughput at one thread,
// 0.82 at four), so a client delegates only where contention makes it
// worth it. The runtime keeps a gate for each mutex, its own, beside the
// mutex (an entry, below): the count of clients in a section of it as
// written, and whether its server is in a busy period (from a request it
// finds to the queue found empty). What a client does with a section
// depends on the mutex's mode:
//
//   - cold (every mutex starts so): a client that finds the gate open, no
//     such client and the server idle, takes it and runs fn on its own
//     thread: the section as written, which takes the mutex itself; that
//     is the runtime's trylock. One that finds the gate taken runs fn as
//     written as well, waiting for the mutex inside it, and times it, the
//     wait for the mutex and the section. The gate keeps a moving average
//     of what a section of the mutex waits, a free section counting as no
//     wait (weight 1/4096, kept as a sum in fixed point so that no time is
//     lost to rounding at any counter rate; each time counted at most
//     eight times the threshold; a thread keeps its count of free sections
//     for each of the last four mutexes it ran, and adds a count when it
//     next adds a time to that mutex or drops the mutex), and when it
//     passes WTC_DELEGATE_HOT_NS (750 ns unless set, between what four and
//     eight contending threads wait for LevelDB's DB mutex on the leg's
//     host), the mutex turns hot. A time
//     taken while the server is still in a busy period, running sections
//     delegated before the mutex turned cold, is not counted;
//   - hot: a client delegates every section, starting the mutex's server
//     if it has none; the gate is not looked at, since a section run on
//     its client between two busy periods would take the lines the server
//     keeps. After every WTC_DELEGATE_EPOCH sections (262144 unless set)
//     the server turns the mutex cold and its average back to half the
//     threshold: the clients measure afresh, and it turns hot again only
//     if waiting for the mutex still costs more than the threshold; when
//     it does not, the server, given no more work, ends. A hot mutex whose
//     server is given nothing for kLinger (10 milliseconds) turns cold too,
//     whatever is left of its epoch, and one for which no server can be
//     had turns cold at once.
//
// So a mutex that is never contended never starts a server, and one that
// stops being contended gives its server up. WTC_DELEGATE_MODE=always
// delegates every section, free or not, as the runtime measured on 24 Nov
// did, and
// WTC_DELEGATE_HOT_NS=0 delegates every section that finds the gate taken
// and runs the others free, with no estimate.
//
// The gate is a hint, never a lock: no client waits for it, it only tries
// it, and exclusion is the mutex's, which fn takes wherever it runs. A
// server that starts a busy period while clients are in sections as
// written waits for them a little (kGateSpinNs, 10 microseconds) before it
// runs its first request, rather than sleep in the kernel on the mutex
// they hold or wait for; past that it runs it, and fn waits for the mutex
// as any thread would.
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
// section of it is delegated. While one that has turned cold is ending,
// answering the requests its clients were already handing it, a second
// may start for the same mutex, if it turns hot again: two threads that
// take one mutex, which orders nothing the original did not.
//
// The server takes the mutex itself, inside fn, one section at a time, as
// the original section does; a client that runs a section on its own
// thread, by the gate or for any reason below, or a section of the same
// mutex the compiler did not delegate, takes it as before, and exclusion
// is the original's whichever way each section runs.
//
// Who runs inline. A server that reaches a delegated section, of its own
// mutex or of another, runs it on its own thread: the server must never
// wait for itself, and on another server it would wait while holding its
// mutex, which the original did on that thread too. Everything else is a
// client. Order: the sections of one client run in its program order (it
// waits for each); between clients they run in the order the mutex grants
// it, which for the delegated ones is the order the queue linearises their
// pushes: an order the original's mutex could have granted.
//
// Happens-before. A section run on its client is the original's. For a
// delegated one, a client's push happens before the server's pop of it
// (release, acquire), so everything the client did before the section is
// visible to it; the server's store of `done` (release) and the client's
// load of it (acquire) make everything the section did visible to the
// client after it, as the original's unlock did. Between sections of
// different clients, wherever each ran, the mutex itself, taken by fn,
// orders them as before.
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
// destructor, runs on it. A server ends when its mutex has turned cold and
// stayed so with nothing for it to do for kLinger (10 milliseconds, long
// enough for the clients of a still contended mutex to measure that and
// turn it hot again), so that its place goes to the next mutex that turns
// hot. It takes itself out of its mutex's entry, then answers every
// request until no client says it is handing it one: a client says so
// before it reads the entry again and hands the section over, both
// sequentially consistent, so either it sees the server gone or the server
// sees it. Then the server is freed. When the last client ends, every
// server is woken and ends too, so that a program whose main thread ends
// with pthread_exit ends when its last own thread does, as written; a
// later client starts them again. At most WTC_DELEGATE_MAX_SERVERS (4 unless set) run at once:
// the sections of a mutex delegated when as many are running run on their
// client, under the mutex, as written, and so do they when a server cannot
// be started (pthread_create failing for want of memory or threads). That
// bounds the threads a class with one mutex per object can start, and
// since a server starts only when its mutex turns hot, the servers go to
// the contended mutexes; a thread that runs a section as written is always
// faithful, it only gains nothing. Gates are kept for at most kEntries
// (512) mutexes, never freed; the sections of any further one run as
// written, without the registry's lock.
//
// What a server is started with. Every signal blocked: an asynchronous
// signal must go to a thread of the program's, never to the server, whose
// mask would otherwise be that of the client that happened to start it. A
// stack as large as that client's (at least the default, at most 1 GiB):
// a section never has less stack on the server than it had on that
// client, and a client whose own stack is larger than its server's runs
// its sections itself. The affinity and scheduling of the client that
// starts it, unless WTC_DELEGATE_CPU pins it. A process that forks gets no
// server in the child: the child's runtime starts empty (pthread_atfork,
// registered while the program starts), every mutex cold, and its first
// delegated section starts a server of its own. No state of the runtime's
// is a function-local static with a dynamic initialiser, whose guard a
// fork could leave taken.
//
// errno. The section does not touch it (the compiler proves so), and the
// runtime keeps it: the client's errno after a section is what it was
// before, whatever the runtime's own calls did to it.
//
// Environment, read once, at the first section:
//
//   WTC_DELEGATE_MODE=always      delegate every section, free or not;
//                                 unset or anything else, by the gate and
//                                 the mutex's mode (above)
//   WTC_DELEGATE_HOT_NS=N         a mutex turns hot when its sections
//                                 wait N ns on average, a free one
//                                 counting as none (default 750); 0
//                                 delegates every section that finds the
//                                 gate taken
//   WTC_DELEGATE_EPOCH=N          a server turns its mutex cold after
//                                 every N sections (default 262144)
//   WTC_DELEGATE_CPU=LIST         pin the servers, in the order they start,
//                                 to these CPUs (a comma-separated list,
//                                 used round robin); unset, a server takes
//                                 its starting client's affinity
//   WTC_DELEGATE_WAIT=spin        the servers never park when idle
//   WTC_DELEGATE_SPIN_NS=N        spin this long before parking (clients,
//                                 servers)
//   WTC_DELEGATE_MAX_SERVERS=N    at most N servers at once (default 4)
//   WTC_DELEGATE_REPORT=1         print at exit each server's count of
//                                 sections, the clients' counts of where
//                                 theirs ran, and each gate's average
//
// A thread started inside a delegated section inherits the server's
// signal mask (every signal blocked), affinity and scheduling: what its
// starting client had when it started the server (docs/delegation.md,
// "Assumptions", assume-uniform-threads).

#ifndef WTC_RT_DELEGATE_HPP
#define WTC_RT_DELEGATE_HPP

#include <pthread.h>
#include <sched.h>
#include <signal.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
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

    // fn(env) on this thread while the mutex at `key` is cold, on its
    // server while it is hot (or always, in that mode), and on this thread
    // whenever this thread is a server, is ending, has more stack than the
    // server, or no server can be had; its result either way.
    static int run(const void* key, section fn, void* env) {
        thread_state& me = state();
        if (me.on_server || me.exiting) return fn(env);
        if (me.counted_gen != generation().load(std::memory_order_acquire)) {
            const errno_kept kept;
            if (!become_client(me)) return fn(env);
        }
        if (me.last_key != key || me.gen != generation().load(std::memory_order_acquire)) look_up(key, me);
        entry* e = me.gate;
        if (!e) return fn(env);
        const bool always = cfg_always();
        const std::uint64_t hot_ns = cfg_hot_ns();
        if (!always && !(hot_ns && e->hot.load(std::memory_order_relaxed))) {
            if (e->in.load(std::memory_order_relaxed) == 0 && e->busy.load(std::memory_order_relaxed) == 0) {
                std::uint32_t none = 0;
                if (e->in.compare_exchange_strong(none, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
                    const int result = fn(env);
                    e->in.fetch_sub(1, std::memory_order_release);
                    ++me.tally.free;
                    ++me.keys[me.cur].free_run;
                    return result;
                }
            }
            if (hot_ns) return as_written(*e, fn, env, me);
        }
        return delegate(*e, fn, env, me);
    }

    // The servers started so far in this process, ended ones included, and
    // the sections each has run, in the order they started: for tests and
    // the report.
    static std::vector<std::uint64_t> served() {
        registry& g = reg();
        std::lock_guard<std::mutex> lock(g.m);
        std::vector<std::uint64_t> out;
        for (const auto* c : g.served) out.push_back(c->load(std::memory_order_relaxed));
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

    // Where a client's sections ran: on it with the gate open (free), on
    // it as written with the gate taken (contended), on a server
    // (delegated). Sections run as written for any other reason (no
    // server to be had, a thread ending) are not counted.
    struct counts {
        std::uint64_t free = 0;
        std::uint64_t contended = 0;
        std::uint64_t delegated = 0;
    };

    // The calling thread's counts, and those of every client of this
    // process (the ended ones and the calling thread): for tests and the
    // report.
    static counts mine() { return state().tally; }
    static counts totals() {
        registry& g = reg();
        counts t;
        {
            std::lock_guard<std::mutex> lock(g.m);
            t = g.ended_tally;
        }
        add(t, state().tally);
        return t;
    }

    // Whether the mutex at `key` is hot now (false when it has no gate).
    static bool hot(const void* key) {
        entry* e = find_entry(reg(), key, /*insert=*/false);
        return e && e->hot.load(std::memory_order_relaxed);
    }

    // The gate's average, as run() keeps it: the sum after one more
    // section that took `took` ticks (0 for a free one) and, before it,
    // `free_before` free sections; `sum >> kShift` is the average in
    // ticks. Public for the tests, which check it against the threshold at
    // other counter rates than this host's.
    static constexpr unsigned kShift = 12;
    static std::uint64_t next_sum(std::uint64_t sum, std::uint64_t took, std::uint64_t free_before) {
        sum = decayed(sum, free_before);
        return sum - (sum >> kShift) + took;
    }
    // sum (1 - 2^-kShift)^k, the weight k free sections leave it: exact to
    // a double's rounding, and never more for a larger k. exp2 of a
    // moderate negative number sets no errno; past 2^20 sections the
    // weight is under 2^-369 and the sum zero.
    static std::uint64_t decayed(std::uint64_t sum, std::uint64_t k) {
        if (!k || !sum) return sum;
        if (k >= (std::uint64_t{1} << 20)) return 0;
        constexpr double kLog2Step = 0.0003522634716290214;  // -log2(1 - 1/4096)
        return static_cast<std::uint64_t>(static_cast<double>(sum) * std::exp2(-static_cast<double>(k) * kLog2Step));
    }
    static constexpr bool over(std::uint64_t sum, std::uint64_t threshold_ticks) {
        return sum > (threshold_ticks << kShift);
    }

private:
    // The section does not touch errno (the compiler proves it), and
    // neither may what runs it: the original's lock and unlock leave it as
    // it was, where the runtime's own calls (the stack's size, a server's
    // start, the futex a client parks on) may set it.
    struct errno_kept {
        const int saved = errno;
        ~errno_kept() { errno = saved; }
    };

    // How long a client sleeps at most before it looks again (see
    // "Waiting" above).
    static constexpr std::chrono::nanoseconds kParkLimit{1000000};
    // How long a server given nothing keeps its mutex hot, and then how
    // long the server of a cold mutex waits for it to turn hot again before
    // it ends (see "The servers' lives").
    static constexpr std::chrono::nanoseconds kLinger{10000000};
    static constexpr std::size_t kMaxStack = std::size_t{1} << 30;
    // How long a server waits for the clients in sections as written
    // before it runs a request (see "Where a section runs").
    static constexpr std::uint64_t kGateSpinNs = 10000;
    // The gates: a table of kTable slots, open addressing, filled up to
    // kEntries mutexes, half of it, so that a probe always ends at an empty
    // slot.
    static constexpr std::size_t kTable = 1024;
    static constexpr unsigned kEntries = 512;
    // The most one section's time counts in the average, as a multiple of
    // the threshold.
    static constexpr std::uint64_t kClip = 8;
    // The mutexes a thread keeps free counts for.
    static constexpr unsigned kKeys = 4;

    // The configuration, read from the environment on first use. No state
    // of the runtime's is a function-local static with a dynamic
    // initialiser: its guard is a lock, and a fork taken while another
    // thread is inside the initialiser leaves the child waiting on it for
    // ever. These are constant-initialised atomics, set by whichever
    // thread gets there first; two that race set the same values.
    static inline std::atomic<bool> cfg_read_{false};
    static inline std::atomic<bool> cfg_always_{false};
    static inline std::atomic<std::uint64_t> cfg_hot_ns_{0};
    static inline std::atomic<std::uint64_t> cfg_epoch_{0};
    static inline std::atomic<std::uint64_t> hot_ticks_{0};
    static inline std::atomic<std::uint64_t> gate_ticks_{0};

    static void read_cfg() {
        if (cfg_read_.load(std::memory_order_acquire)) return;
        const char* m = std::getenv("WTC_DELEGATE_MODE");
        const char* h = std::getenv("WTC_DELEGATE_HOT_NS");
        const char* p = std::getenv("WTC_DELEGATE_EPOCH");
        std::uint64_t epoch = p && *p ? std::strtoull(p, nullptr, 10) : std::uint64_t{1} << 18;
        cfg_always_.store(m && std::strcmp(m, "always") == 0, std::memory_order_relaxed);
        cfg_hot_ns_.store(h && *h ? std::strtoull(h, nullptr, 10) : 750, std::memory_order_relaxed);
        cfg_epoch_.store(epoch ? epoch : 1, std::memory_order_relaxed);
        cfg_read_.store(true, std::memory_order_release);
    }
    static bool cfg_always() {
        read_cfg();
        return cfg_always_.load(std::memory_order_relaxed);
    }
    static std::uint64_t cfg_hot_ns() {
        read_cfg();
        return cfg_hot_ns_.load(std::memory_order_relaxed);
    }
    static std::uint64_t cfg_epoch() {
        read_cfg();
        return cfg_epoch_.load(std::memory_order_relaxed);
    }

    static std::uint64_t ns_to_ticks(std::uint64_t ns) {
        const std::uint64_t per_ms = ticks_per_ms();
        const std::uint64_t t = ns / 1000000 * per_ms + ns % 1000000 * per_ms / 1000000;
        return t ? t : 1;
    }
    // In ticks, measured at the first section run as written under
    // contention, or the first busy period of a server that waits for
    // one; at least one tick, on a counter as slow as 24 MHz.
    static std::uint64_t hot_ticks() {
        std::uint64_t t = hot_ticks_.load(std::memory_order_relaxed);
        if (!t) {
            t = ns_to_ticks(cfg_hot_ns());
            hot_ticks_.store(t, std::memory_order_relaxed);
        }
        return t;
    }
    static std::uint64_t gate_spin_ticks() {
        std::uint64_t t = gate_ticks_.load(std::memory_order_relaxed);
        if (!t) {
            t = ns_to_ticks(kGateSpinNs);
            gate_ticks_.store(t, std::memory_order_relaxed);
        }
        return t;
    }

    struct request {
        section fn = nullptr;
        void* env = nullptr;
        waiter* client = nullptr;
        int result = 0;
        std::atomic<bool> done{false};
    };

    struct server;

    // A mutex's gate and mode. `in` and `busy` are read by every client at
    // every section while the mutex is cold, and written by the clients
    // that run sections as written and by the server once per busy period;
    // the average, the mode and the server are written only on the
    // contended paths and once an epoch, on a line of their own.
    struct entry {
        explicit entry(const void* k) : key(k) {}
        const void* key;
        alignas(64) std::atomic<std::uint32_t> in{0};    // clients in a section as written
        std::atomic<std::uint32_t> busy{0};              // servers in a busy period (two, while one ends)
        alignas(64) std::atomic<std::uint64_t> sum{0};   // the average, in ticks << kShift (next_sum)
        std::atomic<bool> hot{false};
        std::atomic<server*> srv{nullptr};               // its running server; set and cleared under the registry's lock
    };

    struct server {
        server(entry* e, wait_policy p, std::uint64_t spin) : home(e), idle(p, spin) {}
        entry* home;
        segment_queue<request*, /*kManyConsumers=*/false> queue;
        alignas(64) waiter idle;
        std::atomic<std::uint64_t>* served = nullptr;  // its count, kept in the registry
        std::atomic<bool> stop{false};
        std::size_t stack = 0;
        pthread_t thread{};
    };

    struct thread_state;

    struct registry {
        registry() {
            for (auto& slot : table) slot.store(nullptr, std::memory_order_relaxed);
        }
        std::mutex m;
        std::vector<std::atomic<std::uint64_t>*> served;  // one per server started, never freed
        std::vector<server*> servers;                     // running
        std::vector<thread_state*> clients;               // live clients, for a server that ends
        unsigned started = 0;
        unsigned active = 0;      // servers running
        unsigned live = 0;        // client threads that have not ended
        bool read_env = false;
        bool spin = false;
        bool report = false;
        unsigned max_servers = 4;
        std::vector<int> cpus;
        counts ended_tally;       // of the clients that have ended
        std::atomic<unsigned> entries{0};
        // A slot goes from null to an entry once, under the lock, and is
        // read without it.
        std::atomic<entry*> table[kTable];
    };

    // Trivially destructible: readable from any thread-local destructor.
    struct thread_state {
        bool on_server = false;
        bool exiting = false;
        unsigned gen = 0;            // the registry generation the fields below belong to
        unsigned counted_gen = ~0u;  // the generation whose live count includes this thread
        const void* last_key = nullptr;
        entry* gate = nullptr;       // the last mutex's gate, or null when it has none
        // The last kKeys mutexes this thread ran sections of, each with the
        // free sections it ran since it last counted in that mutex's average:
        // a thread that goes between a few mutexes keeps each one's count,
        // and one it drops is counted into that mutex's average then.
        struct key_count {
            const void* key = nullptr;
            entry* gate = nullptr;
            std::uint64_t free_run = 0;
        };
        key_count keys[kKeys];
        unsigned cur = 0;            // keys[cur] is last_key's
        unsigned next_out = 0;       // the slot a new key takes
        std::atomic<server*> pushing{nullptr};  // the server this client is handing a section to
        waiter* client = nullptr;
        std::size_t stack = 0;
        counts tally;
    };

    static void add(counts& t, const counts& c) {
        t.free += c.free;
        t.contended += c.contended;
        t.delegated += c.delegated;
    }

    static thread_state& state() {
        static thread_local thread_state me;
        return me;
    }

    // A fork's child starts with an empty runtime: a new registry and pool
    // (the old ones, whose locks another thread may have held, are left).
    static inline std::atomic<unsigned> generation_{0};
    static inline std::atomic<registry*> registry_{nullptr};
    static std::atomic<unsigned>& generation() { return generation_; }
    template <class T>
    static std::atomic<T*>& made(std::atomic<T*>& p) {
        if (!p.load(std::memory_order_acquire)) {
            T* fresh = new T;
            T* none = nullptr;
            if (!p.compare_exchange_strong(none, fresh, std::memory_order_acq_rel, std::memory_order_acquire))
                delete fresh;
        }
        return p;
    }
    static std::atomic<registry*>& reg_ptr() { return made(registry_); }
    static registry& reg() { return *reg_ptr().load(std::memory_order_acquire); }

    struct waiter_pool {
        std::mutex m;
        std::vector<waiter*> free;
    };
    static inline std::atomic<waiter_pool*> pool_{nullptr};
    static std::atomic<waiter_pool*>& pool_ptr() { return made(pool_); }

    static void after_fork_in_child() {
        reg_ptr().store(new registry, std::memory_order_release);
        pool_ptr().store(new waiter_pool, std::memory_order_release);
        generation().fetch_add(1, std::memory_order_acq_rel);
    }
    // Registered while the program starts, before any thread can hold the
    // registry's or the pool's lock: registered later, by the first
    // client, a fork taken before it would leave the child with the
    // parent's runtime, its locks perhaps held by threads the child has
    // not.
    static bool hook_fork() noexcept {
        pthread_atfork(nullptr, nullptr, after_fork_in_child);
        return true;
    }
    static inline const bool fork_hooked_ = hook_fork();

    // A client's waiter, from a pool that is never freed: a thread that
    // ends gives its waiter back, and a later thread takes it over. The
    // lease's destructor is the client's end.
    struct waiter_lease {
        waiter* w = nullptr;
        ~waiter_lease() {
            thread_state& me = state();
            me.exiting = true;
            if (me.counted_gen == generation().load(std::memory_order_acquire)) client_ended(me);
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
        read_cfg();
        registry& g = reg();
        std::lock_guard<std::mutex> lock(g.m);
        read_env(g);
        ++g.live;
        g.clients.push_back(&me);
        me.counted_gen = gen;
        me.gen = gen;
        me.last_key = nullptr;
        me.gate = nullptr;
        // The counts of the registry before a fork are not this one's.
        for (auto& k : me.keys) k = thread_state::key_count{};
        me.cur = 0;
        return true;
    }

    // A client has ended: its counts are kept, and when it was the last,
    // every server is told to end.
    static void client_ended(thread_state& me) {
        registry& g = reg();
        std::lock_guard<std::mutex> lock(g.m);
        add(g.ended_tally, me.tally);
        for (auto it = g.clients.begin(); it != g.clients.end(); ++it)
            if (*it == &me) {
                g.clients.erase(it);
                break;
            }
        if (g.live == 0 || --g.live != 0) return;
        for (server* s : g.servers) {
            s->stop.store(true, std::memory_order_release);
            s->idle.notify(1);
        }
    }

    // Read on first use; ~0 until then (see the configuration above).
    static inline std::atomic<std::uint64_t> spin_ns_{~std::uint64_t{0}};
    static std::uint64_t spin_ns() {
        std::uint64_t ns = spin_ns_.load(std::memory_order_relaxed);
        if (ns == ~std::uint64_t{0}) {
            const char* e = std::getenv("WTC_DELEGATE_SPIN_NS");
            ns = e && *e ? std::strtoull(e, nullptr, 10) : std::uint64_t{50000};
            if (ns == ~std::uint64_t{0}) --ns;
            spin_ns_.store(ns, std::memory_order_relaxed);
        }
        return ns;
    }

    static inline std::atomic<bool> hooked_{false};

    // With the registry's lock held.
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
        // Once per process, not per registry; with an atomic rather than
        // std::call_once, whose flag a fork could leave taken.
        if (g.report && !hooked_.exchange(true, std::memory_order_relaxed)) std::atexit(report);
    }

    static void report() {
        registry& g = reg();
        std::lock_guard<std::mutex> lock(g.m);
        unsigned i = 0;
        for (const auto* c : g.served)
            std::fprintf(stderr, "wtc-delegate: server %u ran %llu sections\n", i++,
                         static_cast<unsigned long long>(c->load(std::memory_order_relaxed)));
        counts t = g.ended_tally;
        add(t, state().tally);
        std::fprintf(stderr, "wtc-delegate: clients: %llu sections, free=%llu contended=%llu delegated=%llu\n",
                     static_cast<unsigned long long>(t.free + t.contended + t.delegated),
                     static_cast<unsigned long long>(t.free), static_cast<unsigned long long>(t.contended),
                     static_cast<unsigned long long>(t.delegated));
        for (auto& slot : g.table) {
            entry* e = slot.load(std::memory_order_acquire);
            if (!e || !e->sum.load(std::memory_order_relaxed)) continue;
            const std::uint64_t per_ms = ticks_per_ms();
            std::fprintf(stderr, "wtc-delegate: mutex %p %s, waiting for it takes %llu ns a section\n", e->key,
                         e->hot.load(std::memory_order_relaxed) ? "hot" : "cold",
                         static_cast<unsigned long long>((e->sum.load(std::memory_order_relaxed) >> kShift) *
                                                         1000000 / per_ms));
        }
    }

    // The gate of `key`, made if there is none and room for one (when
    // `insert`); null otherwise. Open addressing over the whole table,
    // which is never more than half full, so a probe ends at an empty slot
    // or the key.
    static entry* find_entry(registry& g, const void* key, bool insert) {
        const std::size_t h =
            static_cast<std::size_t>((reinterpret_cast<std::uintptr_t>(key) >> 4) * 0x9e3779b97f4a7c15ull >> 40);
        for (std::size_t i = 0; i < kTable; ++i) {
            entry* x = g.table[(h + i) % kTable].load(std::memory_order_acquire);
            if (!x) break;
            if (x->key == key) return x;
        }
        // No gate, and no room for one: no lock taken.
        if (!insert || g.entries.load(std::memory_order_acquire) >= kEntries) return nullptr;
        const errno_kept kept;  // operator new may set ENOMEM
        std::lock_guard<std::mutex> lock(g.m);
        for (std::size_t i = 0; i < kTable; ++i) {
            std::atomic<entry*>& slot = g.table[(h + i) % kTable];
            entry* x = slot.load(std::memory_order_relaxed);
            if (x && x->key == key) return x;
            if (x) continue;
            if (g.entries.load(std::memory_order_relaxed) >= kEntries) return nullptr;
            x = new (std::nothrow) entry(key);
            if (!x) return nullptr;
            // The slot first: a thread that reads the count full has then
            // seen it, and finds the gate rather than none.
            slot.store(x, std::memory_order_release);
            g.entries.fetch_add(1, std::memory_order_release);
            return x;
        }
        return nullptr;
    }

    static void look_up(const void* key, thread_state& me) {
        me.last_key = key;
        me.gen = generation().load(std::memory_order_acquire);
        for (unsigned i = 0; i < kKeys; ++i)
            if (me.keys[i].key == key) {
                me.cur = i;
                me.gate = me.keys[i].gate;
                return;
            }
        thread_state::key_count& k = me.keys[me.next_out];
        if (k.gate && k.free_run) count_free(*k.gate, k.free_run);
        k.key = key;
        k.gate = find_entry(reg(), key, /*insert=*/true);
        k.free_run = 0;
        me.cur = me.next_out;
        me.next_out = (me.next_out + 1) % kKeys;
        me.gate = k.gate;
    }

    // The free sections a thread ran of a mutex it no longer keeps a count
    // for, counted into the mutex's average as no wait.
    static void count_free(entry& e, std::uint64_t free_sections) {
        std::uint64_t sum = e.sum.load(std::memory_order_relaxed);
        while (!e.sum.compare_exchange_weak(sum, decayed(sum, free_sections), std::memory_order_relaxed)) {
        }
    }

    // A section of a cold mutex that found the gate taken: run as written,
    // its time (the wait for the mutex and the section) counted into the
    // gate's average with the free sections this thread ran since it last
    // counted, which turns the mutex hot when the average passes the
    // threshold.
    static int as_written(entry& e, section fn, void* env, thread_state& me) {
        e.in.fetch_add(1, std::memory_order_relaxed);
        const std::uint64_t t0 = ticks();
        const int result = fn(env);
        const std::uint64_t took = ticks() - t0;
        e.in.fetch_sub(1, std::memory_order_release);
        ++me.tally.contended;
        const std::uint64_t free_before = me.keys[me.cur].free_run;
        me.keys[me.cur].free_run = 0;
        // While the server is still running the sections delegated before
        // the mutex turned cold, a wait measures the server, not the mutex
        // as written: it is not counted.
        if (e.busy.load(std::memory_order_relaxed)) return result;
        // A time of more than kClip times the threshold is a wait for a
        // long holder (a section the compiler did not delegate), which
        // delegating would not shorten: it counts as that much, so that one
        // long wait cannot turn the mutex hot. A compare-and-swap, so that
        // the server's reset at the end of an epoch is never lost.
        const std::uint64_t limit = kClip * hot_ticks();
        const std::uint64_t counted = took < limit ? took : limit;
        std::uint64_t sum = e.sum.load(std::memory_order_relaxed), next;
        do {
            next = next_sum(sum, counted, free_before);
        } while (!e.sum.compare_exchange_weak(sum, next, std::memory_order_relaxed));
        if (over(next, hot_ticks()) && !e.hot.load(std::memory_order_relaxed)) e.hot.store(true, std::memory_order_relaxed);
        return result;
    }

    // A section handed to the mutex's server, started if it has none.
    static int delegate(entry& e, section fn, void* env, thread_state& me) {
        const errno_kept kept;
        server* s = nullptr;
        for (int tries = 0; tries < 4 && !s; ++tries) {
            s = e.srv.load(std::memory_order_acquire);
            if (!s) s = server_of(e, me);
            if (!s) break;
            // The server may be ending (serve): it clears e.srv and then
            // waits for every client that says it is handing it a section.
            // Saying so, then reading e.srv again, both sequentially
            // consistent, the client either sees it cleared, and leaves
            // the server alone, or is seen by it.
            me.pushing.store(s, std::memory_order_seq_cst);
            if (e.srv.load(std::memory_order_seq_cst) != s) {
                me.pushing.store(nullptr, std::memory_order_release);
                s = nullptr;
            }
        }
        if (!s) return fn(env);
        if (me.stack > s->stack) {
            me.pushing.store(nullptr, std::memory_order_release);
            return fn(env);
        }
        request r;
        r.fn = fn;
        r.env = env;
        r.client = me.client;
        s->queue.push(&r);
        s->idle.notify(1);
        auto done = [&] { return r.done.load(std::memory_order_acquire); };
        while (!me.client->wait_for(done, kParkLimit)) {
        }
        me.pushing.store(nullptr, std::memory_order_release);
        ++me.tally.delegated;
        return r.result;
    }

    // The running server of the mutex, started if there is none and room
    // for one; null when its sections run on their clients. A hot mutex
    // that can have no server turns cold, and measures again.
    static server* server_of(entry& e, thread_state& me) {
        registry& g = reg();
        server* s = nullptr;
        {
            std::lock_guard<std::mutex> lock(g.m);
            s = e.srv.load(std::memory_order_relaxed);
            if (!s && g.active < g.max_servers) s = start(g, e, me.stack);
        }
        if (!s && !cfg_always() && cfg_hot_ns()) {
            e.sum.store(0, std::memory_order_relaxed);
            e.hot.store(false, std::memory_order_relaxed);
        }
        return s;
    }

    // With the registry's lock held.
    static server* start(registry& g, entry& e, std::size_t client_stack) {
        server* s = new (std::nothrow) server(&e, g.spin ? wait_policy::spin : wait_policy::spin_then_park, spin_ns());
        if (!s) return nullptr;
        auto* count = new (std::nothrow) std::atomic<std::uint64_t>(0);
        if (!count) {
            delete s;
            return nullptr;
        }
        s->served = count;
        pthread_attr_t a;
        pthread_attr_init(&a);
        std::size_t size = 0;
        pthread_attr_getstacksize(&a, &size);
        if (client_stack > size) size = client_stack < kMaxStack ? client_stack : kMaxStack;
        if (pthread_attr_setstacksize(&a, size) == 0) s->stack = size;
        else pthread_attr_getstacksize(&a, &s->stack);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        // Every signal blocked from its first instruction: inherited from
        // this thread's mask, set for the call. The server is published
        // (e.srv) once it runs, still under the lock, which it needs
        // before it can end and take itself out again.
        sigset_t all, old;
        sigfillset(&all);
        pthread_sigmask(SIG_SETMASK, &all, &old);
        const int err = pthread_create(&s->thread, &a, serve, s);
        pthread_sigmask(SIG_SETMASK, &old, nullptr);
        pthread_attr_destroy(&a);
        if (err) {
            delete count;
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
        g.served.push_back(count);
        g.servers.push_back(s);
        e.srv.store(s, std::memory_order_release);
        ++g.started;
        ++g.active;
        return s;
    }

    // Whether a client says it is handing a section to s; with the lock.
    static bool used(registry& g, server* s) {
        for (thread_state* c : g.clients)
            if (c->pushing.load(std::memory_order_seq_cst) == s) return true;
        return false;
    }

    // Runs one request and answers its client.
    static void answer(server* s, request* r) {
        const int result = r->fn(r->env);
        waiter* client = r->client;
        r->result = result;
        // After this store the request, on the client's stack, may be
        // gone; the client's waiter is not (waiter_pool).
        r->done.store(true, std::memory_order_release);
        if (client->waiting()) client->notify(1);
        s->served->store(s->served->load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    // The server ends: it is taken out of its entry, then it answers every
    // request until no client says it is handing it one (see delegate),
    // and then it is freed. With no client left (the last has ended) that
    // is at once.
    static void end(registry& g, server* s, entry& e) {
        {
            std::lock_guard<std::mutex> lock(g.m);
            server* mine = s;
            e.srv.compare_exchange_strong(mine, nullptr, std::memory_order_seq_cst);
        }
        request* r = nullptr;
        for (;;) {
            bool any;
            {
                std::lock_guard<std::mutex> lock(g.m);
                any = used(g, s);
            }
            while (s->queue.try_pop(r)) answer(s, r);
            if (!any) break;
            relax();
        }
        while (s->queue.try_pop(r)) answer(s, r);
        std::lock_guard<std::mutex> lock(g.m);
        for (auto it = g.servers.begin(); it != g.servers.end(); ++it)
            if (*it == s) {
                g.servers.erase(it);
                break;
            }
        --g.active;
        delete s;
    }

    // The mutex turns cold, its average at half the threshold (see "Where
    // a section runs").
    static void cool(entry& e) {
        e.sum.store((hot_ticks() << kShift) / 2, std::memory_order_relaxed);
        e.hot.store(false, std::memory_order_relaxed);
    }

    static void* serve(void* arg) noexcept {
        server* s = static_cast<server*>(arg);
        entry& e = *s->home;
        registry& g = reg();
        state().on_server = true;
        const bool adaptive = !cfg_always() && cfg_hot_ns();
        request* r = nullptr;
        bool busy = false;  // in a busy period: from a request found to the queue found empty
        const std::uint64_t epoch = cfg_epoch();
        std::uint64_t to_epoch = epoch;  // sections left in this epoch
        auto pop = [&] { return s->queue.try_pop(r); };
        for (;;) {
            if (!pop()) {
                if (busy) {
                    e.busy.fetch_sub(1, std::memory_order_release);
                    busy = false;
                }
                auto work = [&] { return pop() || s->stop.load(std::memory_order_acquire); };
                if (adaptive) {
                    // Given nothing for kLinger: a hot mutex has stopped
                    // being contended, whatever is left of its epoch, and
                    // turns cold; a cold one, whose clients are measuring
                    // again, gives its server up, and the place goes to the
                    // next mutex that turns hot.
                    if (!s->idle.wait_for(work, kLinger)) {
                        if (e.hot.load(std::memory_order_relaxed)) {
                            cool(e);
                            to_epoch = epoch;
                        } else {
                            end(g, s, e);
                            return nullptr;
                        }
                        continue;
                    }
                } else {
                    s->idle.wait(work);
                }
                if (!r) {
                    // Told to end: end if no client is left, else go on.
                    bool last;
                    {
                        std::lock_guard<std::mutex> lock(g.m);
                        s->stop.store(false, std::memory_order_relaxed);
                        last = g.live == 0;
                    }
                    if (last) {
                        end(g, s, e);
                        return nullptr;
                    }
                    continue;
                }
            }
            if (!busy) {
                busy = true;
                e.busy.fetch_add(1, std::memory_order_relaxed);
                // Clients in sections as written hold the mutex or wait
                // for it: let them through first, a little, rather than
                // sleep on it.
                if (e.in.load(std::memory_order_acquire)) {
                    const std::uint64_t until = ticks() + gate_spin_ticks();
                    while (e.in.load(std::memory_order_acquire) && static_cast<std::int64_t>(until - ticks()) > 0)
                        relax();
                }
            }
            answer(s, r);
            r = nullptr;
            // An epoch is over: the mutex turns cold and its average starts
            // again from half the threshold, so that its clients measure
            // afresh what waiting for it costs, and it turns hot again once
            // that is still more than the threshold: after a few hundred
            // sections where it is several times the threshold, never
            // where it is under half.
            if (--to_epoch == 0) {
                to_epoch = epoch;
                if (e.hot.load(std::memory_order_relaxed)) cool(e);
            }
        }
        return nullptr;
    }

    static void relax() {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
    }
};

}  // namespace wtc::rt

#endif
