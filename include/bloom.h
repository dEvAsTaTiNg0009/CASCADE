#pragma once
// =============================================================================
// bloom.h — Blocked Bloom Filter (cache-line aligned, 64-byte blocks)
#include <memory>
#include <mutex>
//
// Each block = 64 bytes = 512 bits.
// A probe maps key to ONE block (single cache-line load) then sets/checks k bits.
// This eliminates random cache misses from standard Bloom filters.
// Dual-trigger reallocation: structural (compaction) + frequency (Merlin).
// =============================================================================
#include "common.h"
#include <vector>
#include <cstring>

namespace cascade {

class BlockedBloomFilter {
public:
    static constexpr int BLOCK_BYTES = 64;
    static constexpr int BLOCK_BITS  = BLOCK_BYTES * 8; // 512

    explicit BlockedBloomFilter(int total_bits = 8192, int k = 8);

    void add(Key key);
    bool possiblyContains(Key key) const;
    // Resize the filter (clears all bits, resets element count).
    // Does NOT update k_ — use rebuild(bits, k) when the optimal hash count
    // changes alongside the size (e.g. after reallocateStructural).
    void rebuild(int new_total_bits);
    // Resize the filter AND update the number of hash functions.
    // Use this when bits-per-key changes so k stays optimal.
    void rebuild(int new_total_bits, int new_k);
    void clear();

    int  totalBits()   const { return (int)blocks_.size() * 8; }
    int  hashCount()   const { return k_; }  // number of hash functions (k_)
    int  numHashes()   const { return k_; }  // alias for hashCount()
    int  elementCount() const { return n_; }
    int  numElements() const { return n_; }
    double fpr()       const; // theoretical FPR = (1 - e^(-k*n/m))^k

    std::vector<uint8_t> serialize() const;
    static BlockedBloomFilter deserialize(const uint8_t* data, size_t size);

private:
    int                       k_;    // number of hash functions
    int                       n_;    // number of elements added
    // Blocks stored contiguously; each block is 64 bytes
    std::vector<uint8_t>      blocks_; // size = num_blocks * BLOCK_BYTES

    int numBlocks() const { return (int)blocks_.size() / BLOCK_BYTES; }

    // Map key to block index + k bit positions within that block
    void probe(Key key, int& block_idx, uint16_t bits[]) const;
};

// ---------------------------------------------------------------------------
// Per-SSTable access frequency tracker for Merlin/Mnemosyne sliding window
// ---------------------------------------------------------------------------
struct SStableAccessTracker {
    std::vector<int> access_counts;
    int  window_size = 1000;
    int  ops_since_reset = 0;
    std::shared_ptr<std::mutex> mu = std::make_shared<std::mutex>();

    void recordAccess(int sstable_idx) {
        std::lock_guard<std::mutex> lk(*mu);
        if (sstable_idx < (int)access_counts.size())
            access_counts[sstable_idx]++;
        if (++ops_since_reset >= window_size) resetLocked();
    }
    void resize(int n) {
        std::lock_guard<std::mutex> lk(*mu);
        access_counts.assign(n, 0);
    }
    void reset() {
        std::lock_guard<std::mutex> lk(*mu);
        resetLocked();
    }
    void resetLocked() {
        access_counts.assign(access_counts.size(), 0);
        ops_since_reset = 0;
    }
    int hottest() const {
        std::lock_guard<std::mutex> lk(*mu);
        int best = 0;
        for (int c : access_counts) best = std::max(best, c);
        return best;
    }
};

// ---------------------------------------------------------------------------
// Dual-trigger adaptive allocation across all levels
//
// Structural trigger (reallocateStructural):
//   Each level i gets: max(512, bits_per_key * |L_i| * depth_mult_i) bits
//   depth_mult_i = 1 + 0.1*i  (Monkey-style: deeper levels earn more bits/key)
//   Capped to bloom_max_bytes total (safety valve).
//
// Frequency trigger (reallocateFrequency):
//   Hot levels (high access count) get a +10% per-key bonus on top.
// ---------------------------------------------------------------------------
class SSTable;

struct BloomAllocator {
    // ---------------------------------------------------------------------------
    // allocateWithBudget — centralized budget-preserving allocator.
    //
    // Computes a per-level bit allocation that:
    //   (a) Uses normalized weights: w_i = f_i * d_i / (sum_j f_j * d_j)
    //       where f_i = |L_i| / sum_j |L_j|  and  d_i = 1 + 0.1*i
    //   (b) Pre-assigns b_min = 512 bits per level (one Bloom block).
    //   (c) Distributes remaining budget proportional to w_i (water-filling).
    //   (d) Guarantees: every b_i >= b_min, sum(b_i) <= total_budget_bits.
    //   (e) If L * b_min > total_budget_bits, emits a warning and falls back
    //       to b_min for every level (cannot conserve budget in this case).
    // ---------------------------------------------------------------------------
    static std::vector<int64_t> allocateWithBudget(
        const std::vector<int>& counts,    // entry count per level
        int64_t total_budget_bits,
        int bits_per_key,
        const std::vector<double>& freq_factors = {}); // optional per-level freq multiplier

    // Structural trigger: called after AHLC compaction changes level shapes
    static void reallocateStructural(
        std::vector<BlockedBloomFilter>& filters,
        const std::vector<std::vector<std::vector<KVPair>>>& levels,
        int bits_per_key,         // bits per key per level (primary driver)
        size_t max_total_bytes);  // safety cap (e.g. 256 MB)

    static void reallocateStructural(
        std::vector<BlockedBloomFilter>& filters,
        const std::vector<std::vector<std::shared_ptr<SSTable>>>& levels,
        int bits_per_key,
        size_t max_total_bytes);

    // Frequency trigger: micro-boost hot levels (Merlin-style).
    // Frequency information modifies allocation WEIGHTS (not added on top):
    //   w_i = f_i * d_i * freq_factor_i
    // followed by the SAME normalized/water-filled allocator.
    // This preserves the global budget regardless of frequency distribution.
    static void reallocateFrequency(
        std::vector<BlockedBloomFilter>& filters,
        const std::vector<SStableAccessTracker>& trackers,
        const std::vector<std::vector<std::vector<KVPair>>>& levels,
        int bits_per_key);

    static void reallocateFrequency(
        std::vector<BlockedBloomFilter>& filters,
        const std::vector<SStableAccessTracker>& trackers,
        const std::vector<std::vector<std::shared_ptr<SSTable>>>& levels,
        int bits_per_key);
};

} // namespace cascade
