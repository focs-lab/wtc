/******************************************************************************
 * Copyright (c) 2022, Raed Romanov
 * Based on FAAArrayQueue implementation by Pedro Ramalhete and Andreia Correia:
 *
 * Copyright (c) 2014-2016, Pedro Ramalhete, Andreia Correia
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Concurrency Freaks nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 ******************************************************************************
 */

#ifndef _FAA_ARRAY_QUEUE_HP_H_
#define _FAA_ARRAY_QUEUE_HP_H_

#include <atomic>
#include <cstring>
#include <stdexcept>
#include <cmath>
#include "RQCell.hpp"
#include "Metrics.hpp"
#include "HazardPointers.hpp"
#include "CacheRemap.hpp"


/**
 * <h1> Fetch-And-Add Array Queue </h1>
 *
 * Each node has one array but we don't search for a vacant entry. Instead, we
 * use FAA to obtain an index in the array, for enqueueing or dequeuing.
 *
 * There are some similarities between this queue and the basic queue in YMC:
 * http://chaoran.me/assets/pdf/wfq-ppopp16.pdf
 * but it's not the same because the queue in listing 1 is obstruction-free, while
 * our algorithm is lock-free.
 * In FAAArrayQueue eventually a new node will be inserted (using Michael-Scott's
 * algorithm) and it will have an item pre-filled in the first position, which means
 * that at most, after BUFFER_SIZE steps, one item will be enqueued (and it can then
 * be dequeued). This kind of progress is lock-free.
 *
 * Each entry in the array may contain one of three possible values:
 * - A valid item that has been enqueued;
 * - nullptr, which means no item has yet been enqueued in that position;
 * - taken, a special value that means there was an item but it has been dequeued;
 *
 * Enqueue algorithm: FAA + CAS(null,item)
 * Dequeue algorithm: FAA + CAS(item,taken)
 * Consistency: Linearizable
 * enqueue() progress: lock-free
 * dequeue() progress: lock-free
 * Memory Reclamation: Hazard Pointers (lock-free)
 * Uncontended enqueue: 1 FAA + 1 CAS + 1 HP
 * Uncontended dequeue: 1 FAA + 1 CAS + 1 HP
 *
 *
 * <p>
 * Lock-Free Linked List as described in Maged Michael and Michael Scott's paper:
 * {@link http://www.cs.rochester.edu/~scott/papers/1996_PODC_queues.pdf}
 * <a href="http://www.cs.rochester.edu/~scott/papers/1996_PODC_queues.pdf">
 * Simple, Fast, and Practical Non-Blocking and Blocking Concurrent Queue Algorithms</a>
 * <p>
 * The paper on Hazard Pointers is named "Hazard Pointers: Safe Memory
 * Reclamation for Lock-Free objects" and it is available here:
 * http://web.cecs.pdx.edu/~walpole/class/cs510/papers/11.pdf
 */
template<typename T, bool padded_cells = true, int BUFFER_SIZE = 1024, bool cache_remap = true>
class FAAArrayQueue : public MetricsAwareBase {
private:
    using Cell = detail::PlainCell<T*, padded_cells>;
    constexpr static ConditionalCacheRemap<cache_remap, BUFFER_SIZE, sizeof(Cell)> remap{};

    struct Node {
        alignas(128) std::atomic<int>   deqidx;
        alignas(128) std::atomic<int>   enqidx;
        alignas(128) std::atomic<Node*> next;
        Cell                            items[BUFFER_SIZE];
        const uint64_t startIndexOffset;

        // Start with the first entry pre-filled and enqidx at 1
        Node(T* item, uint64_t startIndexOffset) : deqidx{0}, enqidx{1}, next{nullptr},
            startIndexOffset(startIndexOffset) {
            std::memset(items, 0, sizeof(items));
            items[remap[0]].val.store(item, std::memory_order_relaxed);
        }

        bool casNext(Node *cmp, Node *val) {
            return next.compare_exchange_strong(cmp, val);
        }
    };

    bool casTail(Node *cmp, Node *val) {
		return tail.compare_exchange_strong(cmp, val);
	}

    bool casHead(Node *cmp, Node *val) {
        return head.compare_exchange_strong(cmp, val);
    }

    // Pointers to head and tail of the list
    alignas(128) std::atomic<Node*> head;
    alignas(128) std::atomic<Node*> tail;

    static constexpr int MAX_THREADS = 128;
    const int maxThreads;

    T* taken = (T*)new int();  // Muuuahahah !

    // We need just one hazard pointer
    HazardPointers<Node> hp {2, maxThreads};
    const int kHpTail = 0;
    const int kHpHead = 1;

    MetricsCollector::Accessor mAppendNode = accessor("appendNode");
    MetricsCollector::Accessor mWasteNode = accessor("wasteNode");


public:
    static constexpr size_t RING_SIZE = BUFFER_SIZE;

    FAAArrayQueue(int maxThreads=MAX_THREADS)
            : MetricsAwareBase(maxThreads), maxThreads{maxThreads} {
        Node* sentinelNode = new Node(nullptr, 0);
        sentinelNode->enqidx.store(0, std::memory_order_relaxed);
        head.store(sentinelNode, std::memory_order_relaxed);
        tail.store(sentinelNode, std::memory_order_relaxed);
        mAppendNode.inc(1, 0);
    }


    ~FAAArrayQueue() {
        while (dequeue(0) != nullptr); // Drain the queue
        delete head.load();            // Delete the last node
        delete (int*)taken;
    }


    static std::string className() {
        using namespace std::string_literals;
        return "FAAArrayQueue"s + (padded_cells ? "/ca"s : ""s) + (cache_remap ? "/remap" : "");
    }

    size_t estimateSize(int tid) {
        Node* lhead = hp.protect(kHpHead, head, tid);
        Node* ltail = hp.protect(kHpTail, tail, tid);
        uint64_t t = std::min((uint64_t) RING_SIZE, (uint64_t) ltail->enqidx.load()) + ltail->startIndexOffset;
        uint64_t h = std::min((uint64_t) RING_SIZE, (uint64_t) lhead->deqidx.load()) + lhead->startIndexOffset;
        hp.clear(tid);
        return t > h ? t - h : 0;
    }

    void enqueue(T* item, const int tid) {
        if (item == nullptr)
            throw std::invalid_argument("item can not be nullptr");
        while (true) {
            Node* ltail = hp.protect(kHpTail, tail, tid);
            const int idx = ltail->enqidx.fetch_add(1);
            if (idx > BUFFER_SIZE-1) { // This node is full
                if (ltail != tail.load()) continue;
                Node* lnext = ltail->next.load();
                if (lnext == nullptr) {
                    Node* newNode = new Node(item, ltail->startIndexOffset + BUFFER_SIZE);
                    if (ltail->casNext(nullptr, newNode)) {
                        casTail(ltail, newNode);
                        hp.clearOne(kHpTail, tid);
                        mAppendNode.inc(1, tid);
                        return;
                    }
                    mWasteNode.inc(1, tid);
                    delete newNode;
                } else {
                    casTail(ltail, lnext);
                }
                continue;
            }
            T* itemnull = nullptr;
            if (ltail->items[remap[idx]].val.compare_exchange_strong(itemnull, item)) {
                hp.clearOne(kHpTail, tid);
                return;
            }
        }
    }


    T* dequeue(const int tid) {
        T* item = nullptr;
        Node* lhead = hp.protect(kHpHead, head, tid);

#ifdef CAUTIOUS_DEQUEUE
        if (lhead->deqidx.load() >= lhead->enqidx.load() && lhead->next.load() == nullptr)
            return nullptr;
#endif

        while (true) {
            const int idx = lhead->deqidx.fetch_add(1);
            if (idx > BUFFER_SIZE-1) { // This node has been drained, check if there is another one
                Node* lnext = lhead->next.load();
                if (lnext == nullptr) {
                    break;  // No more nodes in the queue
                }
                if (casHead(lhead, lnext))
                    hp.retire(lhead, tid);

                lhead = hp.protect(kHpHead, head, tid);
                continue;
            }
            Cell& cell = lhead->items[remap[idx]];
            if (cell.val.load() == nullptr && idx < lhead->enqidx.load()) {
                for (size_t i = 0; i < 4*1024; ++i) {
                    if (cell.val.load() != nullptr)
                        break;
                }
            }
            item = cell.val.exchange(taken);
            if (item != nullptr)
                break;

            int t = lhead->enqidx.load();
            if (idx + 1 >= t) {
                if (lhead->next.load() != nullptr)
                    continue;
                lhead->enqidx.compare_exchange_strong(t, idx + 1);
                break;
            }
        }
        hp.clearOne(kHpHead, tid);
        return item;
    }
};

#endif /* _FAA_ARRAY_QUEUE_HP_H_ */
