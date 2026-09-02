#pragma once
// =============================================================================
// csb_tree.h — Concurrent CSB+ Tree MemTable with Optimistic Lock Coupling (OLC)
// =============================================================================
#include "common.h"
#include "epoch.h"
#include <atomic>
#include <vector>
#include <functional>
#include <thread>
#include <shared_mutex>

namespace cascade {

static constexpr int CSB_ORDER = 15;

struct alignas(64) CSBNode {
    std::atomic<uint64_t> version{0};  // bit0 = 1 (locked), bit0 = 0 (unlocked)
    uint16_t numKeys  = 0;
    bool     isLeaf   = true;
    Key      keys[CSB_ORDER];
    Value*   vals[CSB_ORDER];
    CSBNode* childGroup = nullptr;

    CSBNode() {
        std::fill(keys, keys + CSB_ORDER, Key(0));
        std::fill(vals, vals + CSB_ORDER, nullptr);
    }

    CSBNode& operator=(const CSBNode& o) {
        if (this == &o) return *this;
        version.store(0, std::memory_order_relaxed);
        numKeys    = o.numKeys;
        isLeaf     = o.isLeaf;
        childGroup = o.childGroup;
        std::copy(o.keys, o.keys + CSB_ORDER, keys);
        std::copy(o.vals, o.vals + CSB_ORDER, vals);
        return *this;
    }

    uint64_t readVersion() const { return version.load(std::memory_order_acquire); }
    static bool isLocked(uint64_t v) { return (v & 1ULL) != 0; }
    bool validate(uint64_t v) const {
        return (version.load(std::memory_order_acquire) == v) && !isLocked(v);
    }
    void setVersion(uint64_t v) {
        version.store(v, std::memory_order_release);
    }
};

class ConcurrentCSBTree {
public:
    explicit ConcurrentCSBTree();
    ~ConcurrentCSBTree();

    void   insert(Key key, const Value& value);
    bool   search(Key key, Value& out_value, bool& is_tombstone);
    void   del(Key key);
    std::vector<KVPair> scan(Key start, Key end);
    std::vector<KVPair> flush();
    bool   isFull(int cap) const;
    int    size() const;

    static inline std::atomic<uint64_t> total_lock_wait_ns_{0};
    static inline std::atomic<uint64_t> total_lock_acquisitions_{0};
    static void resetLockStats() {
        total_lock_wait_ns_.store(0);
        total_lock_acquisitions_.store(0);
    }

private:
    CSBNode*              root_;
    std::atomic<int>      count_{0};
    int                   height_{0};

    mutable std::shared_mutex rw_mu_;      // Synchronizes concurrent writer splits
    mutable SpinLock      alloc_lock_;
    std::vector<CSBNode*> all_groups_;

    CSBNode* allocGroup(int n);
    void     deferFreeGroup(CSBNode* g);
    void     insertLeaf(CSBNode* leaf, Key key, const Value& value);
    CSBNode* findLeaf(Key key) const;
    void     collectAll(CSBNode* node, int depth, std::vector<KVPair>& out) const;
    void     freeAll();
};

} // namespace cascade
