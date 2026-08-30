#include <memory>
#include <mutex>
#pragma once
// =============================================================================
// bloom.h — Blocked Bloom Filter (cache-line aligned, 64-byte blocks)
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
    void rebuild(int new_total_bits);  // structural reallocation
    void clear();

    int  totalBits()   const { return (int)blocks_.size() * BLOCK_BITS; }
    int  numHashes()   const { return k_; }
    int  numElements() const { return n_; }
    double fpr()       const; // theoretical FPR = (1 - e^(-k*n/m))^k

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
// ---------------------------------------------------------------------------
struct BloomAllocator {
    // Structural trigger: called after AHLC compaction changes level shapes
    static void reallocateStructural(
        std::vector<BlockedBloomFilter>& filters,
        const std::vector<std::vector<std::vector<KVPair>>>& levels,
        int total_budget_bits);

    // Frequency trigger: micro-boost hot SSTables from cold ones
    static void reallocateFrequency(
        std::vector<BlockedBloomFilter>& filters,
        const std::vector<SStableAccessTracker>& trackers,
        const std::vector<std::vector<std::vector<KVPair>>>& levels,
        int total_budget_bits);
};

} // namespace cascade
