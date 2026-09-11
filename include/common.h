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

// Selects the memtable implementation used by LSMEngine.
// CSB_PLUS  — Concurrent CSB+ Tree (cache-line aligned, OLC, epoch-GC)
// SKIP_LIST — Concurrent SkipList  (fine-grained spinlocks, simpler but
//             higher cache miss rate due to pointer chasing)
enum class MemtableType { CSB_PLUS, SKIP_LIST };

// Engine configuration
struct Config {
    int          memtable_capacity      = 4096;
    int          max_levels             = 7;

    // Bloom filter sizing — per-key model (Monkey-optimal per-level allocation)
    // Each level i gets: max(512, bloom_bits_per_key * |L_i| * depth_mult_i) bits
    // depth_mult_i = 1 + 0.1*i  (deeper levels get more bits/key)
    //
    // FPR guide for Blocked Bloom (512-bit block, empirical ~7x standard overhead):
    //   bpk=10, k=7  -> std 0.82%, measured blocked ~6%  (not suitable for research claims)
    //   bpk=14, k=10 -> std 0.12%, measured blocked ~1%  ← DEFAULT (good for publication)
    //   bpk=20, k=14 -> std 0.007%,measured blocked ~0.05% (high accuracy, more RAM)
    int          bloom_bits_per_key     = 14;  // bits per key per level (14 → FPR ≈ 1%)
    size_t       bloom_max_bytes        = 256ULL * 1024 * 1024; // 256MB safety cap
    int          bloom_total_budget     = 2000000; // kept for compat; NOT used by allocator

    double       ahlc_write_rate_high   = 5000.0;
    double       ahlc_skew_threshold    = 0.65;
    int          ahlc_hysteresis_epochs = 3;
    double       ahlc_ewma_alpha        = 0.3;
    size_t       block_cache_capacity   = 64ULL * 1024 * 1024;
    int          wal_group_commit_batch = 64;
    double       ssd_write_ns_per_byte  = 2.0;
    double       ssd_read_ns_per_byte   = 0.5;
    int          bytes_per_kv           = 72;
    std::string  db_path                = "./data";
    MemtableType memtable_type          = MemtableType::CSB_PLUS;

    // Page-cache control for controlled experiments.
    // Linux:  O_DIRECT | O_SYNC on SSTable file opens (requires 512-byte aligned buffers;
    //         4KB SSTable blocks are already aligned).
    // macOS:  fcntl(fd, F_NOCACHE, 1) applied after open() — not O_DIRECT (unsupported).
    // When false (default), the OS page cache is used normally.
    bool         direct_io              = false;
};

// Optimal k (number of hash functions) for a given bits-per-key budget.
// k_opt = ln(2) * m/n ≈ 0.693 * bits_per_key
// Clamped to [1, 20] — beyond k=20 the blocked layout shows diminishing gains.
inline int optimalBloomK(int bits_per_key) {
    int k = static_cast<int>(0.693147 * bits_per_key + 0.5);
    return std::max(1, std::min(k, 20));
}

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
