#pragma once
// =============================================================================
// cache.h — Two-tier cache hierarchy
//   1. LRU Block Cache: caches 4KB SSTable data blocks
//   2. Table Cache:     caches open SSTable handles + filter bitsets
// =============================================================================
#include "common.h"
#include <list>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <memory>

namespace cascade {

// ---------------------------------------------------------------------------
// Generic LRU Cache
// ---------------------------------------------------------------------------
template<typename K, typename V>
class LRUCache {
public:
    explicit LRUCache(size_t capacity_bytes)
        : capacity_(capacity_bytes), used_(0) {}

    bool get(const K& key, V& out) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = map_.find(key);
        if (it == map_.end()) { misses_++; return false; }
        list_.splice(list_.begin(), list_, it->second);
        out = it->second->second;
        hits_++;
        return true;
    }

    void put(const K& key, V value, size_t size_bytes) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            used_ -= it->second->value_size;
            list_.erase(it->second);
            map_.erase(it);
        }
        list_.push_front({key, value, size_bytes});
        map_[key] = list_.begin();
        used_ += size_bytes;

        while (used_ > capacity_ && !list_.empty()) {
            auto& back = list_.back();
            used_ -= back.value_size;
            map_.erase(back.key);
            list_.pop_back();
            evictions_++;
        }
    }

    void invalidate(const K& key) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = map_.find(key);
        if (it == map_.end()) return;
        used_ -= it->second->value_size;
        list_.erase(it->second);
        map_.erase(it);
    }

    uint64_t hits()      const { return hits_; }
    uint64_t misses()    const { return misses_; }
    uint64_t evictions() const { return evictions_; }
    double   hitRate()   const {
        uint64_t t = hits_ + misses_;
        return t ? (double)hits_ / t : 0.0;
    }

private:
    struct Node { K key; V second; size_t value_size; };
    size_t capacity_, used_;
    std::list<Node> list_;
    std::unordered_map<K, typename std::list<Node>::iterator> map_;
    mutable std::mutex mu_;
    uint64_t hits_ = 0, misses_ = 0, evictions_ = 0;
};

// ---------------------------------------------------------------------------
// Block Cache: (sstable_id, block_offset) → raw 4KB block bytes
// ---------------------------------------------------------------------------
using BlockKey = std::pair<uint64_t, uint64_t>;
struct BlockKeyHash {
    size_t operator()(const BlockKey& k) const {
        return std::hash<uint64_t>{}(k.first) ^ (std::hash<uint64_t>{}(k.second) << 32);
    }
};

// We adapt LRUCache with a custom hash
class BlockCache {
public:
    explicit BlockCache(size_t cap_bytes = 64ULL * 1024 * 1024);
    bool get(uint64_t ssid, uint64_t offset, std::vector<uint8_t>& out);
    void put(uint64_t ssid, uint64_t offset, std::vector<uint8_t> block);
    double hitRate() const;
    uint64_t hits() const;
    uint64_t misses() const;

private:
    struct Entry {
        std::vector<uint8_t> data;
    };
    size_t capacity_, used_ = 0;
    using Key2 = BlockKey;
    std::list<std::pair<Key2, Entry>> list_;
    std::unordered_map<Key2, decltype(list_)::iterator, BlockKeyHash> map_;
    mutable std::mutex mu_;
    uint64_t hits_ = 0, misses_ = 0;
};

} // namespace cascade
