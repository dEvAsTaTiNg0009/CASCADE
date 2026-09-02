#pragma once
// =============================================================================
// skiplist_mt.h — Concurrent SkipList MemTable (shared-mutex variant)
//
// Implements the same public interface as ConcurrentCSBTree so LSMEngine
// can use either memtable transparently via cfg.memtable_type.
//
// Design deliberately keeps SkipList characteristics intact:
//   - Pointer-chasing traversal (cache-unfriendly vs. CSB+ contiguous groups)
//   - Per-level forward pointers (fragmented in memory)
//   - Probabilistic height (maxLevel = 12 gives O(log N) expected cost)
//
// Concurrency: shared_mutex — concurrent readers, exclusive writers.
// This matches the production-grade read path cost of a typical SkipList
// (e.g. LevelDB's MemTable) without needing a full lock-free implementation,
// and is sufficient to demonstrate the CSB+ cache-locality advantage.
// =============================================================================
#include "common.h"
#include <shared_mutex>
#include <vector>
#include <random>
#include <algorithm>

namespace cascade {

static constexpr int SL_MAX_LEVEL = 12;
static constexpr double SL_P = 0.25;

struct SLNode {
    Key     key;
    Value*  val;     // nullptr = tombstone
    int     level;
    SLNode* forward[SL_MAX_LEVEL + 1];

    SLNode(Key k, const Value& v, int lvl, bool tombstone = false)
        : key(k), val(tombstone ? nullptr : new Value(v)), level(lvl)
    {
        std::fill(forward, forward + SL_MAX_LEVEL + 1, nullptr);
    }
    ~SLNode() { delete val; }
};

// ---------------------------------------------------------------------------
class SkipListMemtable {
public:
    SkipListMemtable() : current_level_(0), count_(0), rng_(42) {
        head_ = new SLNode(0, "", SL_MAX_LEVEL);
    }

    ~SkipListMemtable() {
        SLNode* n = head_;
        while (n) { SLNode* nx = n->forward[0]; delete n; n = nx; }
    }

    void insert(Key key, const Value& value) {
        std::unique_lock<std::shared_mutex> lk(mu_);
        SLNode* update[SL_MAX_LEVEL + 1];
        SLNode* cur = head_;
        for (int i = current_level_; i >= 0; i--) {
            while (cur->forward[i] && cur->forward[i]->key < key)
                cur = cur->forward[i];
            update[i] = cur;
        }
        cur = cur->forward[0];
        if (cur && cur->key == key) {
            delete cur->val;
            cur->val = new Value(value);
            return;
        }
        int lvl = randomLevel();
        if (lvl > current_level_) {
            for (int i = current_level_ + 1; i <= lvl; i++) update[i] = head_;
            current_level_ = lvl;
        }
        SLNode* node = new SLNode(key, value, lvl);
        for (int i = 0; i <= lvl; i++) {
            node->forward[i] = update[i]->forward[i];
            update[i]->forward[i] = node;
        }
        count_++;
    }

    void del(Key key) {
        std::unique_lock<std::shared_mutex> lk(mu_);
        SLNode* cur = head_;
        for (int i = current_level_; i >= 0; i--)
            while (cur->forward[i] && cur->forward[i]->key < key)
                cur = cur->forward[i];
        cur = cur->forward[0];
        if (cur && cur->key == key) {
            delete cur->val;
            cur->val = nullptr; // mark tombstone in place
        }
    }

    bool search(Key key, Value& out, bool& is_tombstone) {
        std::shared_lock<std::shared_mutex> lk(mu_);
        is_tombstone = false;
        SLNode* cur = head_;
        for (int i = current_level_; i >= 0; i--)
            while (cur->forward[i] && cur->forward[i]->key < key)
                cur = cur->forward[i];
        cur = cur->forward[0];
        if (!cur || cur->key != key) return false;
        if (!cur->val) { is_tombstone = true; return true; }
        out = *cur->val;
        return true;
    }

    std::vector<KVPair> scan(Key start, Key end) {
        std::shared_lock<std::shared_mutex> lk(mu_);
        std::vector<KVPair> result;
        SLNode* cur = head_->forward[0];
        while (cur && cur->key < start) cur = cur->forward[0];
        while (cur && cur->key <= end) {
            if (cur->val) result.push_back({cur->key, *cur->val, false});
            cur = cur->forward[0];
        }
        return result;
    }

    std::vector<KVPair> flush() {
        std::unique_lock<std::shared_mutex> lk(mu_);
        std::vector<KVPair> result;
        result.reserve(count_);
        for (SLNode* n = head_->forward[0]; n; n = n->forward[0])
            result.push_back({n->key, n->val ? *n->val : "", n->val == nullptr});
        // clear
        SLNode* n = head_->forward[0];
        while (n) { SLNode* nx = n->forward[0]; delete n; n = nx; }
        std::fill(head_->forward, head_->forward + SL_MAX_LEVEL + 1, nullptr);
        current_level_ = 0;
        count_ = 0;
        return result; // already sorted (skiplist is ordered)
    }

    bool isFull(int cap) const { return count_.load(std::memory_order_relaxed) >= cap; }
    int  size()          const { return count_.load(std::memory_order_relaxed); }

private:
    SLNode*           head_;
    int               current_level_;
    std::atomic<int>  count_;
    mutable std::shared_mutex mu_;
    std::mt19937_64   rng_;

    int randomLevel() {
        int lvl = 0;
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        while (lvl < SL_MAX_LEVEL && dist(rng_) < SL_P) lvl++;
        return lvl;
    }
};

} // namespace cascade
