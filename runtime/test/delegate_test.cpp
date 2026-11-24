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

#include <atomic>
#include <cerrno>
#include <chrono>
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
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: delegate_test exclusion|results|order|nested|servers|park|churn|ends|main_exit|cap|"
                             "signals|stack|fork|tls_exit|errno [N]\n");
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
    else {
        std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
        return 2;
    }
    if (failures) return 1;
    std::printf("%s: ok\n", mode.c_str());
    return 0;
}
