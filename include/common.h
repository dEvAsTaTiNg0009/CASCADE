#pragma once
// =============================================================================
// common.h — Shared types, constants, and utilities for CASCADE research engine
// =============================================================================
#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include <limits>
#include <atomic>

namespace cascade {

using Key   = uint64_t;
using Value = std::string;

struct KVPair {
    Key   key   = 0;
    Value value = "";
    bool  is_tombstone = false;
    KVPair() = default;
    KVPair(Key k, Value v = "", bool tomb = false) : key(k), value(std::move(v)), is_tombstone(tomb) {}
    bool operator<(const KVPair& o) const { return key < o.key; }
};

static constexpr Key TOMBSTONE = std::numeric_limits<Key>::max();

// Engine configuration
struct Config {
    int    memtable_capacity      = 4096;
    int    max_levels             = 7;
    int    bloom_total_budget     = 2000000;
    double ahlc_write_rate_high   = 5000.0;
    double ahlc_skew_threshold    = 0.65;
    int    ahlc_hysteresis_epochs = 3;
    double ahlc_ewma_alpha        = 0.3;
    size_t block_cache_capacity   = 64ULL * 1024 * 1024;
    int    wal_group_commit_batch = 64;
    double ssd_write_ns_per_byte  = 2.0;
    double ssd_read_ns_per_byte   = 0.5;
    int    bytes_per_kv           = 72;
};

// splitmix64 — excellent avalanche, fast
inline uint64_t hash64(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31; return x;
}
inline uint64_t hash64b(uint64_t x) { return hash64(x ^ 0xdeadbeefcafeULL); }
inline uint64_t nth_hash(uint64_t h1, uint64_t h2, int i) {
    return h1 + static_cast<uint64_t>(i) * h2;
}

// Minimal spinlock
struct SpinLock {
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
    void lock()   { while (flag.test_and_set(std::memory_order_acquire)) {} }
    void unlock() { flag.clear(std::memory_order_release); }
    struct Guard {
        SpinLock& sl;
        explicit Guard(SpinLock& s) : sl(s) { sl.lock(); }
        ~Guard() { sl.unlock(); }
    };
};

} // namespace cascade
