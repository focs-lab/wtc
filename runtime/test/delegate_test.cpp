// Tests of the delegation server (runtime/delegate.hpp), through the entry
// point the compiler's output calls (shim/delegate.cpp).
//
//   delegate_test exclusion [N]   clients and inline threads share a mutex
//   delegate_test results         results and writes come back
//   delegate_test order [N]       each client's sections run in its order
//   delegate_test nested          a section that delegates runs it inline
//   delegate_test servers [N]     one server per mutex
//   delegate_test park [N]        clients and the server asleep in between
//   delegate_test churn [N]       short-lived clients, waiters taken over
//   delegate_test ends            servers end with the last client
//   delegate_test main_exit       main ends with pthread_exit: the process ends
//   delegate_test cap             at most WTC_DELEGATE_MAX_SERVERS servers
//   delegate_test signals         a server blocks every signal
//   delegate_test stack           a server's stack is its starting client's
//   delegate_test fork            the child of a fork starts its own server
//   delegate_test tls_exit        a section from a thread-local destructor
//   delegate_test errno           a delegated section leaves errno as it was
//   delegate_test fork_statics    forks taken while threads start their first
//                                 sections: no child waits on a lock or a
//                                 static's guard a parent thread held
//
// The tests above check delegation itself and run with
// WTC_DELEGATE_MODE=always (runtime/test/CMakeLists.txt); exclusion,
// results, order, park, churn and main_exit run in the default, adaptive,
// mode as well. These check where the adaptive runtime runs a section:
//
//   delegate_test free [N]        a free mutex: on the client, no server
//   delegate_test contended       contended sections as written, then
//                                 delegated once the mutex is hot, then as
//                                 written once it has cooled; the server
//                                 started then, and ending with its clients
//   delegate_test trylock         WTC_DELEGATE_HOT_NS=0: the first contended
//                                 section is delegated
//   delegate_test mix [N]         the three ways mixed, and threads running
//                                 the section as written: exclusion, counts
//   delegate_test fork_cold       a fork's child starts cold, with no
//                                 server, and turns hot with a server of
//                                 its own
//   delegate_test average         the gate's average against the threshold,
//                                 at this host's counter rate and at 24 MHz
//   delegate_test small_hot       WTC_DELEGATE_HOT_NS=60: a mutex whose
//                                 sections wait microseconds turns hot
//   delegate_test slots           WTC_DELEGATE_MAX_SERVERS=1: a cooled
//                                 mutex's server ends and the next hot one
//                                 gets the place; a hot one that can have
//                                 none turns cold
//   delegate_test gates           mutexes whose gates collide all get one,
//                                 up to 512, and no more
//   delegate_test rare            a mutex contended 1% of the time stays
//                                 cold
//   delegate_test rare_alt        the same, the thread going between it and
//                                 a mutex of its own after every section
//   delegate_test idle_hot        WTC_DELEGATE_MAX_SERVERS=1: a mutex hot in
//                                 a burst and then idle cools and gives its
//                                 server up to the next hot one
//
// A section here is what the compiler outlines: the mutex's own lock, the
// body, the unlock, in one function of the environment. Every test checks
// its counts exactly, and under ThreadSanitizer the plain counters inside
// the sections are the race detector's: a section that ran outside the
// mutex, or whose writes the client did not see after run() returned,
// is a report. A lost wake-up leaves a client asleep for ever; each test
// that can see one waits with a deadline and says so.

#include "../delegate.hpp"

#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

extern "C" int wtc_delegate_run(void* key, int (*fn)(void*), void* env);

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

// The state one mutex guards: plain fields, as a program's would be.
struct guarded {
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    long count = 0;
    long sum = 0;
    std::vector<long> log;
};

struct add_env {
    guarded* g;
    long value;
    long before;  // out: the count the section saw
};

int add_section(void* p) {
    add_env* e = static_cast<add_env*>(p);
    pthread_mutex_lock(&e->g->m);
    e->before = e->g->count;
    e->g->count += 1;
    e->g->sum += e->value;
    pthread_mutex_unlock(&e->g->m);
    return e->before % 3;  // the exit, as the compiler's sections return one
}

// Runs a body on n threads and joins them, failing after a deadline: a
// client left asleep is a lost wake-up.
template <class F>
void on_threads(unsigned n, F body, const char* what) {
    std::atomic<unsigned> done{0};
    std::vector<std::thread> ts;
    for (unsigned i = 0; i < n; ++i) ts.emplace_back([&, i] {
        body(i);
        done.fetch_add(1);
    });
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (done.load() < n) {
        if (std::chrono::steady_clock::now() > until) {
            std::fprintf(stderr, "FAIL: %s: lost wake-up: a client never returned\n", what);
            std::_Exit(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (auto& t : ts) t.join();
}

void exclusion(long n) {
    guarded g;
    const unsigned clients = 6, inline_threads = 2;
    on_threads(clients + inline_threads, [&](unsigned t) {
        for (long i = 0; i < n; ++i) {
            add_env e{&g, i, -1};
            int exit;
            // Threads the compiler could not prove clients run the section
            // as written, taking the same mutex the server takes.
            if (t >= clients) exit = add_section(&e);
            else exit = wtc_delegate_run(&g.m, add_section, &e);
            if (exit != e.before % 3 || e.before < 0) check(false, "exclusion: a result came back wrong");
        }
    }, "exclusion");
    check(g.count == static_cast<long>(clients + inline_threads) * n, "exclusion: a section was lost or ran twice");
    check(g.sum == static_cast<long>(clients + inline_threads) * (n * (n - 1) / 2), "exclusion: the sum is wrong");
}

struct out_env {
    guarded* g;
    int in;
    int out;
    char text[32];
};

int out_section(void* p) {
    out_env* e = static_cast<out_env*>(p);
    pthread_mutex_lock(&e->g->m);
    e->out = e->in * 2 + 1;
    std::snprintf(e->text, sizeof e->text, "section %d", e->in);
    e->g->count++;
    pthread_mutex_unlock(&e->g->m);
    return e->in < 0 ? 1 : 0;
}

void results() {
    guarded g;
    for (int i = -5; i < 50; ++i) {
        out_env e{&g, i, 0, {}};
        int exit = wtc_delegate_run(&g.m, out_section, &e);
        check(exit == (i < 0 ? 1 : 0), "results: the exit index");
        check(e.out == i * 2 + 1, "results: an out-parameter");
        check(std::string(e.text) == "section " + std::to_string(i), "results: memory the section wrote");
    }
    check(g.count == 55, "results: the count");
}

struct order_env {
    guarded* g;
    long tagged;
};

int order_section(void* p) {
    order_env* e = static_cast<order_env*>(p);
    pthread_mutex_lock(&e->g->m);
    e->g->log.push_back(e->tagged);
    pthread_mutex_unlock(&e->g->m);
    return 0;
}

void order(long n) {
    guarded g;
    const unsigned clients = 4;
    on_threads(clients, [&](unsigned t) {
        for (long i = 0; i < n; ++i) {
            order_env e{&g, static_cast<long>(t) << 32 | i};
            wtc_delegate_run(&g.m, order_section, &e);
        }
    }, "order");
    std::vector<long> next(clients, 0);
    bool ok = g.log.size() == clients * static_cast<std::size_t>(n);
    for (long x : g.log) {
        const unsigned t = static_cast<unsigned>(x >> 32);
        const long i = x & 0xffffffffL;
        if (t >= clients || i != next[t]) ok = false;
        else ++next[t];
    }
    check(ok, "order: a client's sections ran out of its order");
}

// A section of mutex A that reaches a delegated section of mutex B (and of
// a third one, C, twice): the server of A runs them on its own thread.
struct nested_env {
    guarded* a;
    guarded* b;
    bool inner_on_server;
    bool outer_on_server;
};

int inner_section(void* p) {
    nested_env* e = static_cast<nested_env*>(p);
    pthread_mutex_lock(&e->b->m);
    e->b->count++;
    e->inner_on_server = wtc::rt::delegation::on_server();
    pthread_mutex_unlock(&e->b->m);
    return 0;
}

int outer_section(void* p) {
    nested_env* e = static_cast<nested_env*>(p);
    pthread_mutex_lock(&e->a->m);
    e->a->count++;
    e->outer_on_server = wtc::rt::delegation::on_server();
    wtc_delegate_run(&e->b->m, inner_section, e);
    pthread_mutex_unlock(&e->a->m);
    return 0;
}

void nested() {
    guarded a, b;
    const std::size_t before = wtc::rt::delegation::served().size();
    for (int i = 0; i < 100; ++i) {
        nested_env e{&a, &b, false, false};
        wtc_delegate_run(&a.m, outer_section, &e);
        check(e.outer_on_server && e.inner_on_server, "nested: a section did not run on the server");
    }
    check(a.count == 100 && b.count == 100, "nested: the counts");
    // Only A's server was started: B's sections ran inline on it.
    check(wtc::rt::delegation::served().size() == before + 1, "nested: a second server was started");
}

void servers(long n) {
    const std::size_t before = wtc::rt::delegation::served().size();
    guarded g[3];
    on_threads(6, [&](unsigned t) {
        for (long i = 0; i < n; ++i) {
            add_env e{&g[(t + i) % 3], 1, -1};
            wtc_delegate_run(&g[(t + i) % 3].m, add_section, &e);
        }
    }, "servers");
    for (auto& x : g) check(x.count == 2 * n, "servers: a mutex's count");
    auto s = wtc::rt::delegation::served();
    check(s.size() == before + 3, "servers: not one server per mutex");
    std::uint64_t total = 0;
    for (std::size_t i = before; i < s.size(); ++i) total += s[i];
    check(total == 6 * static_cast<std::uint64_t>(n), "servers: the servers' counts");
}

// Pauses long past the spin budget between sections, so that the server
// parks between them and the client parks while the section runs slowly.
struct slow_env {
    guarded* g;
    unsigned delay_us;
};

int slow_section(void* p) {
    slow_env* e = static_cast<slow_env*>(p);
    pthread_mutex_lock(&e->g->m);
    if (e->delay_us) std::this_thread::sleep_for(std::chrono::microseconds(e->delay_us));
    e->g->count++;
    pthread_mutex_unlock(&e->g->m);
    return 0;
}

void park(long n) {
    guarded g;
    on_threads(3, [&](unsigned t) {
        std::mt19937 rng(t + 1);
        for (long i = 0; i < n; ++i) {
            std::this_thread::sleep_for(std::chrono::microseconds(rng() % 200));
            slow_env e{&g, static_cast<unsigned>(rng() % 3 == 0 ? rng() % 100 : 0)};
            wtc_delegate_run(&g.m, slow_section, &e);
        }
    }, "park");
    check(g.count == 3 * n, "park: the count");
}

// Clients that make a few requests and end, many times over: the waiters
// of ended threads are taken over by later ones.
void churn(long n) {
    guarded g;
    for (long round = 0; round < n; ++round)
        on_threads(4, [&](unsigned) {
            for (int i = 0; i < 10; ++i) {
                add_env e{&g, 1, -1};
                wtc_delegate_run(&g.m, add_section, &e);
            }
        }, "churn");
    check(g.count == 40 * n, "churn: the count");
}


// Waits up to a deadline for a condition the runtime reaches on its own.
template <class F>
bool eventually(F cond) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!cond()) {
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Every server ends once the last client thread has, and a later client
// starts one again.
void ends() {
    guarded g;
    on_threads(4, [&](unsigned) {
        for (int i = 0; i < 1000; ++i) {
            add_env e{&g, 1, -1};
            wtc_delegate_run(&g.m, add_section, &e);
        }
    }, "ends");
    check(eventually([] { return wtc::rt::delegation::running() == 0; }), "ends: a server outlived its clients");
    on_threads(2, [&](unsigned) {
        add_env e{&g, 1, -1};
        wtc_delegate_run(&g.m, add_section, &e);
    }, "ends again");
    check(g.count == 4002, "ends: the count");
    // A server may have ended between the first threads, too, when one
    // ended before the next became a client.
    check(wtc::rt::delegation::served().size() >= 2, "ends: no new server for the later clients");
}

// As C programs end: main lets its own thread end and the process ends with
// the last of its threads. A server left running would keep it alive; the
// test's time limit catches that.
[[noreturn]] void main_exit() {
    static guarded g;
    on_threads(4, [&](unsigned) {
        for (int i = 0; i < 1000; ++i) {
            add_env e{&g, 1, -1};
            wtc_delegate_run(&g.m, add_section, &e);
        }
    }, "main_exit");
    std::printf("main_exit: ok\n");
    std::fflush(stdout);
    pthread_exit(nullptr);
}

struct where_env {
    guarded* g;
    bool on_server;
    bool sigterm_blocked;
    long depth;
};

int where_section(void* p) {
    where_env* e = static_cast<where_env*>(p);
    pthread_mutex_lock(&e->g->m);
    e->on_server = wtc::rt::delegation::on_server();
    sigset_t now;
    pthread_sigmask(SIG_BLOCK, nullptr, &now);
    e->sigterm_blocked = sigismember(&now, SIGTERM) == 1;
    e->g->count++;
    pthread_mutex_unlock(&e->g->m);
    return 0;
}

// More mutexes than servers: the sections of the rest run on their client.
void cap() {
    const unsigned max = 4;  // the default
    std::vector<guarded> gs(10);
    unsigned on_server = 0;
    for (auto& g : gs) {
        where_env e{&g, false, false, 0};
        wtc_delegate_run(&g.m, where_section, &e);
        on_server += e.on_server;
        check(g.count == 1, "cap: a section did not run");
    }
    check(on_server == max, "cap: not as many servers as allowed");
    check(wtc::rt::delegation::running() == max, "cap: more servers than allowed");
}

// The section runs with every signal blocked, whatever the client's mask
// when it started the server.
void signals() {
    guarded g;
    where_env e{&g, false, false, 0};
    wtc_delegate_run(&g.m, where_section, &e);
    check(e.on_server && e.sigterm_blocked, "signals: the server takes signals");
}

// Uses about `depth` kilobytes of stack.
__attribute__((noinline)) long deep(long depth) {
    volatile char frame[1000];
    frame[0] = static_cast<char>(depth);
    return depth ? deep(depth - 1) + frame[0] : 0;
}

int deep_section(void* p) {
    where_env* e = static_cast<where_env*>(p);
    pthread_mutex_lock(&e->g->m);
    e->on_server = wtc::rt::delegation::on_server();
    e->g->sum += deep(e->depth);
    e->g->count++;
    pthread_mutex_unlock(&e->g->m);
    return 0;
}

// A client with a 64 MB stack: a server it starts has as much, and one a
// default client started runs its sections on it instead.
void stack() {
    guarded small, big;
    where_env first{&small, false, false, 0};
    wtc_delegate_run(&small.m, where_section, &first);  // a server of the default size
    check(first.on_server, "stack: the first section did not run on a server");
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64u << 20);
    struct arg {
        guarded *small, *big;
        bool small_on_server, big_on_server;
    } x{&small, &big, true, false};
    pthread_t t;
    pthread_create(&t, &a, [](void* p) -> void* {
        arg* x = static_cast<arg*>(p);
        where_env e{x->small, false, false, 40000};  // 40 MB: more than the default server has
        wtc_delegate_run(&x->small->m, deep_section, &e);
        x->small_on_server = e.on_server;
        where_env f{x->big, false, false, 40000};
        wtc_delegate_run(&x->big->m, deep_section, &f);
        x->big_on_server = f.on_server;
        return nullptr;
    }, &x);
    pthread_join(t, nullptr);
    pthread_attr_destroy(&a);
    check(!x.small_on_server, "stack: a section ran on a server with less stack than its client");
    check(x.big_on_server, "stack: a big client's server did not run its section");
    check(small.count == 2 && big.count == 1, "stack: the counts");
}

// After a fork the child has no server; its first section starts one.
void fork_child() {
    guarded g;
    where_env e{&g, false, false, 0};
    wtc_delegate_run(&g.m, where_section, &e);
    check(e.on_server, "fork: no server before the fork");
    pid_t pid = fork();
    if (pid == 0) {
        where_env c{&g, false, false, 0};
        wtc_delegate_run(&g.m, where_section, &c);
        std::_Exit(c.on_server && g.count == 2 ? 0 : 3);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "fork: the child's section did not run on its own server");
}

// A thread-local object built before the thread's first section is
// destroyed after the thread has stopped being a client: its section runs
// on the thread itself.
struct late {
    guarded* g = nullptr;
    bool* on_server = nullptr;
    ~late() {
        if (!g) return;
        where_env e{g, true, false, 0};
        wtc_delegate_run(&g->m, where_section, &e);
        *on_server = e.on_server;
    }
};

void tls_exit() {
    guarded g;
    bool on_server = true;
    std::thread([&] {
        static thread_local late l;
        l.g = &g;
        l.on_server = &on_server;
        where_env e{&g, false, false, 0};
        wtc_delegate_run(&g.m, where_section, &e);
    }).join();
    check(g.count == 2, "tls_exit: a section did not run");
    check(!on_server, "tls_exit: a section from an ended client ran on a server");
}
// A section the client waits for past its spin and its bounded park (the
// futex wait returns ETIMEDOUT or EAGAIN), and the thread's first section
// (its stack's size, the server's start): the client's errno is as it was
// before each, as the original's lock and unlock leave it.
void errno_kept() {
    guarded g;
    int changed = 0;
    for (int i = 0; i < 20; ++i) {
        errno = 12345;
        slow_env e{&g, i == 0 ? 0u : 3000u};
        wtc_delegate_run(&g.m, slow_section, &e);
        changed += errno != 12345;
    }
    check(changed == 0, "errno: a delegated section changed the client's errno");
    check(g.count == 20, "errno: the count");
}
// Two threads start delegating, the runtime made on their first
// sections, while the main thread forks children for 3 ms; each child
// delegates from two threads of its own. A child that waits on a lock, or
// on a function-local static's guard, that a parent thread held at the
// fork never ends; its alarm kills it, and that is a failure.
void fork_statics() {
    static guarded parent, child;
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int t = 0; t < 2; ++t) ts.emplace_back([&] {
        while (!stop.load()) {
            add_env e{&parent, 1, -1};
            wtc_delegate_run(&parent.m, add_section, &e);
        }
    });
    std::vector<pid_t> kids;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(3);
    while (std::chrono::steady_clock::now() < until) {
        const pid_t pid = fork();
        if (pid == 0) {
            alarm(10);
            std::thread a([] { for (int i = 0; i < 200; ++i) { add_env e{&child, 1, -1}; wtc_delegate_run(&child.m, add_section, &e); } });
            std::thread b([] { for (int i = 0; i < 200; ++i) { add_env e{&child, 1, -1}; wtc_delegate_run(&child.m, add_section, &e); } });
            a.join();
            b.join();
            std::_Exit(child.count == 400 ? 0 : 3);
        }
        if (pid > 0) kids.push_back(pid);
    }
    stop = true;
    for (auto& t : ts) t.join();
    int bad = 0;
    for (pid_t k : kids) {
        int status = 0;
        waitpid(k, &status, 0);
        bad += !(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    check(!kids.empty(), "fork_statics: no child was forked");
    check(bad == 0, "fork_statics: a child hung or lost a section");
}
// Where a client's section ran, from its counts before and after.
enum class way { free, contended, delegated, other };

way way_of(const wtc::rt::delegation::counts& a, const wtc::rt::delegation::counts& b) {
    const auto f = b.free - a.free, c = b.contended - a.contended, d = b.delegated - a.delegated;
    if (f == 1 && c == 0 && d == 0) return way::free;
    if (f == 0 && c == 1 && d == 0) return way::contended;
    if (f == 0 && c == 0 && d == 1) return way::delegated;
    return way::other;
}

// Waits for a condition another thread of the test makes true, failing
// after a deadline.
template <class F>
void await(F cond, const char* what) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (!cond()) {
        if (std::chrono::steady_clock::now() > until) {
            std::fprintf(stderr, "FAIL: %s: a thread never got there\n", what);
            std::_Exit(1);
        }
        std::this_thread::yield();
    }
}

// A section that says it is inside its mutex and then holds it for a
// while.
struct hold_env {
    guarded* g;
    unsigned hold_us;
    std::atomic<int>* inside;
    int round;
    bool on_server;
};

int hold_section(void* p) {
    hold_env* e = static_cast<hold_env*>(p);
    pthread_mutex_lock(&e->g->m);
    e->on_server = wtc::rt::delegation::on_server();
    e->g->count++;
    if (e->inside) e->inside->store(e->round, std::memory_order_release);
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(e->hold_us);
    while (std::chrono::steady_clock::now() < until) {
    }
    pthread_mutex_unlock(&e->g->m);
    return 0;
}

// Rounds of contention for one mutex, on two threads of their own. In each
// round the holder runs a section that holds the mutex for 200
// microseconds; once it is inside, the contender runs one, which finds the
// gate taken. Between rounds both wait 2 milliseconds, so that the server
// has ended its busy period. each(round, holder's way, contender's way,
// contender's section on a server) says whether to go on; the rounds stop
// after `max` in any case. Returns the rounds run.
template <class F>
int contend(guarded& g, int max, F each) {
    std::atomic<int> go{0}, inside{0}, held{0};
    std::atomic<bool> stop{false};
    way holder_way = way::other;
    std::thread holder([&] {
        for (int r = 1;; ++r) {
            await([&] { return go.load(std::memory_order_acquire) >= r || stop.load(std::memory_order_acquire); },
                  "contend: holder");
            if (stop.load(std::memory_order_acquire)) return;
            const auto a = wtc::rt::delegation::mine();
            hold_env e{&g, 200, &inside, r, false};
            wtc_delegate_run(&g.m, hold_section, &e);
            holder_way = e.on_server ? way::delegated : way_of(a, wtc::rt::delegation::mine());
            held.store(r, std::memory_order_release);
        }
    });
    int rounds = 0;
    std::thread contender([&] {
        for (int r = 1; r <= max; ++r) {
            rounds = r;
            go.store(r, std::memory_order_release);
            await([&] { return inside.load(std::memory_order_acquire) >= r; }, "contend: holder inside");
            const auto a = wtc::rt::delegation::mine();
            hold_env e{&g, 0, nullptr, r, false};
            wtc_delegate_run(&g.m, hold_section, &e);
            const way w = way_of(a, wtc::rt::delegation::mine());
            await([&] { return held.load(std::memory_order_acquire) >= r; }, "contend: holder done");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (!each(r, holder_way, w, e.on_server)) break;
        }
        stop.store(true, std::memory_order_release);
    });
    contender.join();
    holder.join();
    return rounds;
}

// A mutex no other client is in: every section runs on its client, by the
// free path, and no server starts.
void free_path(long n) {
    guarded g;
    const std::size_t before = wtc::rt::delegation::served().size();
    const auto a = wtc::rt::delegation::mine();
    long on_server = 0;
    for (long i = 0; i < n; ++i) {
        where_env e{&g, true, false, 0};
        wtc_delegate_run(&g.m, where_section, &e);
        on_server += e.on_server;
    }
    const auto b = wtc::rt::delegation::mine();
    check(on_server == 0, "free: a section of a free mutex ran on a server");
    check(b.free - a.free == static_cast<std::uint64_t>(n) && b.contended == a.contended && b.delegated == a.delegated,
          "free: a section did not take the free path");
    check(wtc::rt::delegation::served().size() == before, "free: a server started for a free mutex");
    check(!wtc::rt::delegation::hot(&g.m), "free: a free mutex turned hot");
    check(g.count == n, "free: the count");
}

// Cold, the holder's sections run free and the contender's as written,
// and the contender's waits turn the mutex hot; hot, both are delegated,
// starting the server; at the end of an epoch (WTC_DELEGATE_EPOCH=4 in the
// test's environment) the server turns it cold again, and the next round
// runs as the cold ones did. The server ends with the two clients.
void contended() {
    guarded g;
    const std::size_t before = wtc::rt::delegation::served().size();
    bool heated = false, cold_written = true, then = false, cooled = false, after = false, holder_free = true;
    const int rounds = contend(g, 4000, [&](int, way holder, way w, bool on_server) {
        if (!heated || cooled) holder_free = holder_free && holder == way::free;
        // A contender descheduled past the holder's 200 microseconds finds
        // the mutex free: that round shows nothing.
        if (!heated || cooled)
            if (w == way::free) return true;
        if (!heated) {
            cold_written = cold_written && w == way::contended && !on_server;
            heated = wtc::rt::delegation::hot(&g.m);
            return true;
        }
        if (!then) {
            then = w == way::delegated && on_server && holder == way::delegated;
            return then;
        }
        if (!cooled) {
            cooled = !wtc::rt::delegation::hot(&g.m);
            return true;
        }
        after = w == way::contended && !on_server;
        return false;
    });
    check(holder_free, "contended: the holder did not find a cold mutex free");
    check(heated, "contended: the mutex never turned hot");
    check(cold_written, "contended: a cold mutex's contended section did not run as written");
    check(then, "contended: a hot mutex's sections were not delegated");
    check(cooled, "contended: the mutex never turned cold again");
    check(after, "contended: a cold mutex's contended section did not run as written");
    check(g.count == 2 * rounds, "contended: the count");
    check(wtc::rt::delegation::served().size() == before + 1, "contended: not one server started");
    check(eventually([] { return wtc::rt::delegation::running() == 0; }), "contended: the server outlived its clients");
}

// WTC_DELEGATE_HOT_NS=0: no estimate, every section that finds the gate
// taken is delegated, the first one included.
void trylock() {
    guarded g;
    bool delegated = false;
    contend(g, 50, [&](int, way, way w, bool on_server) {
        delegated = w == way::delegated && on_server;
        return w == way::free;  // the round missed the holder (see contended)
    });
    check(delegated, "trylock: a contended section was not delegated");
}

struct mix_env {
    guarded* g;
    long value;
    unsigned hold_us;
    long before;
};

int mix_section(void* p) {
    mix_env* e = static_cast<mix_env*>(p);
    pthread_mutex_lock(&e->g->m);
    e->before = e->g->count;
    e->g->count += 1;
    e->g->sum += e->value;
    if (e->hold_us) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(e->hold_us);
        while (std::chrono::steady_clock::now() < until) {
        }
    }
    pthread_mutex_unlock(&e->g->m);
    return e->before % 3;
}

// Clients whose sections run free, as written under contention and
// delegated, mixed with threads that run the section as written outside
// the runtime: exclusion, results, and every client's section counted
// once. Every fourth section holds the mutex for 5 microseconds, so that
// the clients behind it wait long enough to turn the mutex hot
// (WTC_DELEGATE_HOT_NS=200 in the test's environment), and the mutex
// turns cold again every 256 delegated sections (WTC_DELEGATE_EPOCH).
void mix(long n) {
    guarded g;
    const unsigned clients = 6, inline_threads = 2;
    const auto a = wtc::rt::delegation::totals();
    on_threads(clients + inline_threads, [&](unsigned t) {
        for (long i = 0; i < n; ++i) {
            mix_env e{&g, i, i % 4 == 0 ? 5u : 0u, -1};
            const int exit = t >= clients ? mix_section(&e) : wtc_delegate_run(&g.m, mix_section, &e);
            if (exit != e.before % 3 || e.before < 0) check(false, "mix: a result came back wrong");
        }
    }, "mix");
    const auto b = wtc::rt::delegation::totals();
    const std::uint64_t f = b.free - a.free, c = b.contended - a.contended, d = b.delegated - a.delegated;
    check(g.count == static_cast<long>(clients + inline_threads) * n, "mix: a section was lost or ran twice");
    check(g.sum == static_cast<long>(clients + inline_threads) * (n * (n - 1) / 2), "mix: the sum is wrong");
    check(f + c + d == clients * static_cast<std::uint64_t>(n), "mix: the clients' counts do not add up");
    check(f > 0 && c > 0 && d > 0, "mix: a way of running a section was never taken");
}

// A fork's child: the parent's hot mutex is cold there, with no server,
// and turns hot with a server of the child's own.
void fork_cold() {
    guarded g;
    bool parent = false;
    contend(g, 4000, [&](int, way, way w, bool on_server) {
        parent = w == way::delegated && on_server;
        return !parent;
    });
    check(parent && wtc::rt::delegation::hot(&g.m), "fork_cold: the parent's mutex did not turn hot");
    pid_t pid = fork();
    if (pid == 0) {
        const bool cold = !wtc::rt::delegation::hot(&g.m) && wtc::rt::delegation::running() == 0;
        where_env c{&g, true, false, 0};
        wtc_delegate_run(&g.m, where_section, &c);
        bool child = false;
        contend(g, 4000, [&](int, way, way w, bool on_server) {
            child = w == way::delegated && on_server;
            return !child;
        });
        std::_Exit(cold && !c.on_server && child ? 0 : 3);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "fork_cold: the child did not start cold and turn hot on its own");
}
// The average as run() keeps it (next_sum, over): times a little over and
// a little under the threshold, at this host's counter rate and at 24 MHz
// (an Arm generic timer), with no free sections and with as many free
// sections as contended ones, which halve the average.
void average() {
    for (const std::uint64_t per_ms : {wtc::rt::ticks_per_ms(), std::uint64_t{24000}})
        for (const std::uint64_t ns : {std::uint64_t{60}, std::uint64_t{750}, std::uint64_t{5000}}) {
            const std::uint64_t thr = std::max<std::uint64_t>(1, ns * per_ms / 1000000);
            auto settle = [&](std::uint64_t took, std::uint64_t free_each) {
                std::uint64_t sum = 0;
                for (int i = 0; i < 100000; ++i) sum = wtc::rt::delegation::next_sum(sum, took, free_each);
                return wtc::rt::delegation::over(sum, thr);
            };
            check(settle(thr + thr / 4 + 1, 0), "average: a time 1.25 times the threshold does not turn the mutex hot");
            check(!settle(thr - thr / 4 - 1 > 0 ? thr - thr / 4 - 1 : 0, 0),
                  "average: a time 0.75 times the threshold turns the mutex hot");
            if (thr >= 8) check(!settle(thr + thr / 2, 1), "average: free sections do not count as no wait");
            check(settle(3 * thr, 1), "average: half the sections waiting 3 times the threshold do not turn it hot");
        }
    // k free sections weigh the sum by (1 - 1/4096)^k, never more for a
    // larger k.
    const std::uint64_t big = std::uint64_t{1} << 40;
    std::uint64_t prev = big;
    bool monotonic = true, exact = true;
    for (std::uint64_t k = 0; k < 20000; ++k) {
        const std::uint64_t d = wtc::rt::delegation::decayed(big, k);
        monotonic = monotonic && d <= prev;
        prev = d;
        const double want = static_cast<double>(big) * std::pow(1.0 - 1.0 / 4096, static_cast<double>(k));
        exact = exact && std::fabs(static_cast<double>(d) - want) <= 1e-9 * want + 1;
    }
    check(monotonic, "average: more free sections leave a larger weight");
    check(exact, "average: the weight of free sections is not (1 - 1/4096)^k");
}

// Where it ran, the mutex held `depth` microseconds.
int hold_where_section(void* p) {
    where_env* e = static_cast<where_env*>(p);
    pthread_mutex_lock(&e->g->m);
    e->on_server = wtc::rt::delegation::on_server();
    e->g->count++;
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(e->depth);
    while (std::chrono::steady_clock::now() < until) {
    }
    pthread_mutex_unlock(&e->g->m);
    return 0;
}

// Runs sections of g on `threads` threads, each holding the mutex hold_us
// microseconds, until stop() says so or 20 s pass; returns how many ran on
// a server.
template <class F>
long contend_until(guarded& g, unsigned threads, unsigned hold_us, F stop) {
    std::atomic<bool> done{false};
    std::atomic<long> on_server{0};
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < threads; ++t)
        ts.emplace_back([&] {
            while (!done.load(std::memory_order_relaxed)) {
                where_env e{&g, false, false, hold_us};
                wtc_delegate_run(&g.m, hold_where_section, &e);
                on_server += e.on_server;
            }
        });
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!stop() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    done = true;
    for (auto& t : ts) t.join();
    return on_server.load();
}

// WTC_DELEGATE_HOT_NS=60: the threshold is a few dozen ticks, and the
// waits of a mutex held 5 microseconds by four threads still turn it hot.
void small_hot() {
    guarded g;
    contend_until(g, 4, 5, [&] { return wtc::rt::delegation::hot(&g.m); });
    check(wtc::rt::delegation::hot(&g.m), "small_hot: the mutex never turned hot");
}

// WTC_DELEGATE_MAX_SERVERS=1, WTC_DELEGATE_EPOCH=64: A turns hot and gets
// the one server; uncontended, it cools and its server ends; B, contended
// next, gets the place. While B keeps it, C, contended as well, can have
// no server: it turns cold whenever it turns hot, and its sections run on
// their clients.
void slots() {
    guarded keep, a, b, c;
    add_env k{&keep, 1, -1};
    wtc_delegate_run(&keep.m, add_section, &k);  // this thread stays a client throughout
    contend_until(a, 4, 5, [] { return wtc::rt::delegation::running() == 1; });
    check(wtc::rt::delegation::running() == 1, "slots: A got no server");
    contend_until(a, 1, 0, [&] { return !wtc::rt::delegation::hot(&a.m); });
    check(eventually([] { return wtc::rt::delegation::running() == 0; }), "slots: A's server did not end once A cooled");
    std::atomic<bool> stop{false};
    std::thread hold_b([&] { contend_until(b, 4, 5, [&] { return stop.load(); }); });
    check(eventually([&] { return wtc::rt::delegation::running() == 1 && wtc::rt::delegation::hot(&b.m); }),
          "slots: B, hot after A, got no server");
    const std::size_t started = wtc::rt::delegation::served().size();
    const auto c_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    const auto c_on_server = contend_until(c, 4, 5, [&] { return std::chrono::steady_clock::now() > c_until; });
    // C may have turned hot with its last sections; its next one, with B's
    // server still running, finds no server and turns it cold.
    where_env last{&c, false, false, 0};
    wtc_delegate_run(&c.m, where_section, &last);
    const bool c_hot = wtc::rt::delegation::hot(&c.m);
    stop = true;
    hold_b.join();
    check(c_on_server == 0 && !last.on_server, "slots: a section of C ran on a server while B held the only one");
    check(wtc::rt::delegation::served().size() == started, "slots: a server started past the bound");
    check(!c_hot, "slots: C stayed hot with no server");
}

// Seventeen mutexes whose gates hash to one slot all get one; past 512
// gates, a mutex gets none (its sections run as written, uncounted).
void gates() {
    static guarded pool[1 << 16];
    auto slot_of = [](const void* key) {
        return static_cast<std::size_t>((reinterpret_cast<std::uintptr_t>(key) >> 4) * 0x9e3779b97f4a7c15ull >> 40) % 1024;
    };
    const std::size_t home = slot_of(&pool[0].m);
    std::vector<guarded*> same;
    for (auto& x : pool)
        if (slot_of(&x.m) == home && same.size() < 17) same.push_back(&x);
    auto counted = [](guarded* g) {
        const auto a = wtc::rt::delegation::mine();
        add_env e{g, 1, -1};
        wtc_delegate_run(&g->m, add_section, &e);
        const auto b = wtc::rt::delegation::mine();
        return (b.free - a.free) + (b.contended - a.contended) + (b.delegated - a.delegated) == 1;
    };
    int with_gate = 0;
    for (guarded* g : same) with_gate += counted(g);
    check(same.size() == 17 && with_gate == 17, "gates: a mutex got no gate with far fewer than 512 kept");
    int more = 0;
    for (std::size_t i = 0; i < (1 << 16) && more < 600; ++i)
        if (slot_of(&pool[i].m) != home) more += counted(&pool[i]);
    check(more == 512 - 17, "gates: not exactly 512 gates kept");
}

// One thread runs sections back to back; another holds the mutex 100
// microseconds every 10 milliseconds, 1% of the time. Averaged over every
// section, the first thread's waits are far under the threshold: the
// mutex stays cold and nothing is delegated. With `alternate`, the first
// thread runs a section of a mutex of its own after each, and the second
// holds the mutex 50 microseconds every millisecond, 5% of the time, for
// 2 s: the first thread's free sections of the mutex still count.
void rare(bool alternate = false) {
    static guarded own;
    guarded g;
    std::atomic<bool> stop{false};
    std::uint64_t delegated = 0, all = 0;
    std::thread t1([&] {
        const auto a = wtc::rt::delegation::mine();
        while (!stop.load(std::memory_order_relaxed)) {
            add_env e{&g, 1, -1};
            wtc_delegate_run(&g.m, add_section, &e);
            if (alternate) {
                add_env o{&own, 1, -1};
                wtc_delegate_run(&own.m, add_section, &o);
            }
        }
        const auto b = wtc::rt::delegation::mine();
        delegated = b.delegated - a.delegated;
        all = (b.free - a.free) + (b.contended - a.contended) + delegated;
    });
    std::thread t2([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            mix_env e{&g, 1, alternate ? 50u : 100u, -1};
            wtc_delegate_run(&g.m, mix_section, &e);
            std::this_thread::sleep_for(std::chrono::milliseconds(alternate ? 1 : 10));
        }
    });
    std::this_thread::sleep_for(std::chrono::seconds(alternate ? 2 : 1));
    stop = true;
    t1.join();
    t2.join();
    check(all > 0 && delegated * 100 < all, "rare: a rarely contended mutex delegated");
}

// WTC_DELEGATE_MAX_SERVERS=1: A, contended by four threads until it is hot
// and has the server, then never used again, cools within its server's
// linger, whatever is left of its epoch, and the server ends; B, contended
// next, gets the place.
void idle_hot() {
    guarded keep, a, b;
    add_env k{&keep, 1, -1};
    wtc_delegate_run(&keep.m, add_section, &k);  // this thread stays a client throughout
    contend_until(a, 4, 2, [&] { return wtc::rt::delegation::running() == 1 && wtc::rt::delegation::hot(&a.m); });
    check(wtc::rt::delegation::running() == 1, "idle_hot: A got no server");
    check(eventually([&] { return !wtc::rt::delegation::hot(&a.m) && wtc::rt::delegation::running() == 0; }),
          "idle_hot: A, idle, stayed hot or kept its server");
    std::atomic<bool> got{false};
    contend_until(b, 4, 2, [&] {
        got = got || (wtc::rt::delegation::running() == 1 && wtc::rt::delegation::hot(&b.m));
        return got.load();
    });
    check(got.load(), "idle_hot: B got no server after A went idle");
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: delegate_test exclusion|results|order|nested|servers|park|churn|ends|main_exit|cap|"
                             "signals|stack|fork|tls_exit|errno|fork_statics|free|contended|trylock|mix|fork_cold|average|"
                             "small_hot|slots|gates|rare|rare_alt|idle_hot [N]\n");
        return 2;
    }
    const std::string mode = argv[1];
    const long n = argc > 2 ? std::atol(argv[2]) : 0;
    if (mode == "exclusion") exclusion(n ? n : 100000);
    else if (mode == "results") results();
    else if (mode == "order") order(n ? n : 100000);
    else if (mode == "nested") nested();
    else if (mode == "servers") servers(n ? n : 100000);
    else if (mode == "park") park(n ? n : 2000);
    else if (mode == "churn") churn(n ? n : 200);
    else if (mode == "ends") ends();
    else if (mode == "main_exit") main_exit();
    else if (mode == "cap") cap();
    else if (mode == "signals") signals();
    else if (mode == "stack") stack();
    else if (mode == "fork") fork_child();
    else if (mode == "tls_exit") tls_exit();
    else if (mode == "errno") errno_kept();
    else if (mode == "fork_statics") fork_statics();
    else if (mode == "free") free_path(n ? n : 100000);
    else if (mode == "contended") contended();
    else if (mode == "trylock") trylock();
    else if (mode == "mix") mix(n ? n : 100000);
    else if (mode == "fork_cold") fork_cold();
    else if (mode == "average") average();
    else if (mode == "small_hot") small_hot();
    else if (mode == "slots") slots();
    else if (mode == "gates") gates();
    else if (mode == "rare") rare();
    else if (mode == "rare_alt") rare(true);
    else if (mode == "idle_hot") idle_hot();
    else {
        std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
        return 2;
    }
    if (failures) return 1;
    std::printf("%s: ok\n", mode.c_str());
    return 0;
}
