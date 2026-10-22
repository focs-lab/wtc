// wtc::rt::hazard: hazard pointers for the runtime's queues that free memory
// another thread may still be reading ("Hazard Pointers: Safe Memory
// Reclamation for Lock-Free Objects", IEEE TPDS 2004).
//
// A thread that is about to read a shared node publishes the node's address
// in one of its slots and checks that the node is still reachable; a thread
// that has made a node unreachable retires it, and the node is freed only
// once no slot holds it. The cost to a reader is one sequentially consistent
// store and a reload per protected pointer.
//
// One domain serves the whole process. Each thread that touches a queue
// owns one record, found through a thread_local and kept in a list of
// records that are reused, never freed, so that a scan may walk the list
// while threads come and go. A thread that exits leaves the nodes it retired
// and could not yet free in its record, and whichever thread takes the
// record next frees them in its scans.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

namespace wtc::rt::hazard {

constexpr unsigned kSlots = 2;
// A scan reads every record's slots, so it waits for this many retired
// nodes. The queues retire one segment per thousand-odd elements, so the
// nodes waiting are a few segments at most.
constexpr std::size_t kScanAt = 8;

struct retired {
    void* p;
    void (*free)(void*);
};

struct record {
    std::atomic<void*> slot[kSlots];
    std::atomic<bool> active{true};
    record* next = nullptr;
    std::vector<retired> retired_list;  // its owner's only

    record() {
        for (auto& s : slot) s.store(nullptr, std::memory_order_relaxed);
    }
};

inline std::atomic<record*> g_records{nullptr};

inline record* acquire() {
    for (record* r = g_records.load(std::memory_order_acquire); r; r = r->next) {
        bool idle = false;
        if (!r->active.load(std::memory_order_relaxed) &&
            r->active.compare_exchange_strong(idle, true, std::memory_order_acq_rel))
            return r;
    }
    record* r = new record;
    record* head = g_records.load(std::memory_order_relaxed);
    do {
        r->next = head;
    } while (!g_records.compare_exchange_weak(head, r, std::memory_order_release,
                                              std::memory_order_relaxed));
    return r;
}

// Frees every node this record retired that no slot in any record holds.
inline void scan(record& mine) {
    std::vector<void*> held;
    for (record* r = g_records.load(std::memory_order_acquire); r; r = r->next)
        for (auto& s : r->slot)
            if (void* p = s.load(std::memory_order_seq_cst)) held.push_back(p);
    std::sort(held.begin(), held.end());
    std::vector<retired> keep;
    for (const retired& n : mine.retired_list) {
        if (std::binary_search(held.begin(), held.end(), n.p)) keep.push_back(n);
        else n.free(n.p);
    }
    mine.retired_list.swap(keep);
}

struct owner {
    record* r = acquire();
    ~owner() {
        for (auto& s : r->slot) s.store(nullptr, std::memory_order_release);
        scan(*r);
        r->active.store(false, std::memory_order_release);
    }
};

inline thread_local owner t_owner;

inline record& mine() { return *t_owner.r; }

// Reads src and keeps what it read from being freed until clear(i): the
// value returned was reachable from src after the slot was published, so a
// thread that unlinks it afterwards will find the slot when it scans.
template <class T>
T* protect(record& r, unsigned i, const std::atomic<T*>& src) {
    T* p = src.load(std::memory_order_relaxed);
    for (;;) {
        r.slot[i].store(p, std::memory_order_seq_cst);
        T* again = src.load(std::memory_order_seq_cst);
        if (again == p) return p;
        p = again;
    }
}

inline void clear(record& r, unsigned i) { r.slot[i].store(nullptr, std::memory_order_release); }

// p is unreachable from every shared pointer a reader could protect it
// through; it is freed with free(p) once no slot holds it.
inline void retire(record& r, void* p, void (*free)(void*)) {
    r.retired_list.push_back({p, free});
    if (r.retired_list.size() >= kScanAt) scan(r);
}

}  // namespace wtc::rt::hazard
