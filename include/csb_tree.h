#pragma once
// =============================================================================
// csb_tree.h — Concurrent CSB+ Tree MemTable with Fine-Grained Partitioning
//
// Concurrency Architecture (Phase II — Fine-Grained Partitioning / Per-Subtree Locking):
// ─────────────────────────────────────────────────────────────────────────────
// In earlier versions, a single shared_mutex covered the entire tree. Under 16
// concurrent writer threads, this caused a lock contention bottleneck (Anomaly 1:
// 30.36 µs avg wait/lock vs 24.02 µs for SkipList).
//
// This implementation uses Fine-Grained Partitioning:
//   - The key space is partitioned across NUM_PARTITIONS (32) independent CSB+ subtrees.
//   - Each partition is 64-byte aligned to prevent CPU cache-line false sharing.
//   - Each partition possesses its own shared_mutex, root node, height, and node pool.
//   - Writers lock only the targeted partition's shared_mutex, eliminating cross-thread
//     contention for disjoint key ranges.
//   - Readers (search) take a shared_lock on the single affected partition.
//   - Scans read across partitions under individual shared_locks and merge-sort results.
//   - Flush acquires partition locks in fixed monotonic order (0..N-1) preventing deadlocks,
//     gathers all items, sort-merges, and resets all partition roots.
// =============================================================================
#include "common.h"
#include "epoch.h"
#include <atomic>
#include <vector>
#include <functional>
#include <thread>
#include <shared_mutex>
#include <memory>
#include <algorithm>
#include <chrono>  // always needed for flush timing

// ---------------------------------------------------------------------------
// FlushTimingStats — optional timing measurement for the 32-way partition
// flush and merge operation.  Enabled by compiling with -DCASCADE_FLUSH_TIMING.
// When not defined, the struct is still available but all fields are zero and
// lastFlushStats() returns a zeroed record (no overhead).
// ---------------------------------------------------------------------------
struct FlushTimingStats {
    uint64_t total_flush_time_ns  = 0; // wall time from first lock to last KV returned
    uint64_t merge_sort_time_ns   = 0; // time spent in the final sort-merge across partitions
    uint64_t partition_lock_time_ns = 0; // cumulative time spent acquiring partition locks
    int      num_partitions       = 0; // always NUM_PARTITIONS (32)
    size_t   items_flushed        = 0; // total KVPairs returned
    // Derived: merge_sort_time_ns / total_flush_time_ns gives merge cost fraction
    double mergeFraction() const {
        return (total_flush_time_ns > 0)
            ? (double)merge_sort_time_ns / total_flush_time_ns : 0.0;
    }
};


namespace cascade {

static constexpr int CSB_ORDER = 15;
static constexpr size_t NUM_PARTITIONS = 32;

// ---------------------------------------------------------------------------
// CSBNode
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// CSBPartition — Single 64-byte aligned CSB+ Subtree
// ---------------------------------------------------------------------------
struct alignas(64) CSBPartition {
    mutable std::shared_mutex rw_mu;
    CSBNode*                  root{nullptr};
    int                       height{0};
    std::atomic<int>          count{0};
    mutable SpinLock          alloc_lock;
    std::vector<CSBNode*>     all_groups;

    CSBPartition();
    ~CSBPartition();

    CSBPartition(const CSBPartition&) = delete;
    CSBPartition& operator=(const CSBPartition&) = delete;

    CSBNode* allocGroup(int n);
    void     deferFreeGroup(CSBNode* g);
    void     freeAll();
    void     insertLeaf(CSBNode* leaf, Key key, const Value& value);
    CSBNode* findLeaf(Key key) const;
    void     collectAll(CSBNode* node, int depth, std::vector<KVPair>& out) const;

    bool     insert(Key key, const Value& value);
    bool     search(Key key, Value& out, bool& is_tombstone);
    void     scan(Key start, Key end, std::vector<KVPair>& out);
    void     flush(std::vector<KVPair>& out);
};

// ---------------------------------------------------------------------------
// ConcurrentCSBTree — Main MemTable API
// ---------------------------------------------------------------------------
class ConcurrentCSBTree {
public:
    explicit ConcurrentCSBTree();
    ~ConcurrentCSBTree();

    ConcurrentCSBTree(const ConcurrentCSBTree&) = delete;
    ConcurrentCSBTree& operator=(const ConcurrentCSBTree&) = delete;

    void   insert(Key key, const Value& value);
    bool   search(Key key, Value& out_value, bool& is_tombstone);
    void   del(Key key);
    std::vector<KVPair> scan(Key start, Key end);
    std::vector<KVPair> flush();
    bool   isFull(int cap) const;
    int    size() const;

    // Lock profiling statistics (used by Anomaly 1 benchmark)
    static inline std::atomic<uint64_t> total_lock_wait_ns_{0};
    static inline std::atomic<uint64_t> total_lock_acquisitions_{0};
    static void resetLockStats() {
        total_lock_wait_ns_.store(0);
        total_lock_acquisitions_.store(0);
    }

    // Flush timing stats — populated after each flush() call when
    // -DCASCADE_FLUSH_TIMING is defined.  Always returns a valid struct
    // (zero fields when timing is disabled).
    FlushTimingStats lastFlushStats() const { return last_flush_stats_; }

private:
    std::unique_ptr<CSBPartition[]> partitions_;
    std::atomic<int>                total_count_{0};
    mutable FlushTimingStats        last_flush_stats_; // updated by flush()

    static inline size_t getPartition(Key key) {
        key ^= (key >> 30);
        key *= 0xbf58476d1ce4e5b9ULL;
        key ^= (key >> 27);
        key *= 0x94d049bb133111ebULL;
        key ^= (key >> 31);
        return static_cast<size_t>(key & (NUM_PARTITIONS - 1));
    }
};

} // namespace cascade
