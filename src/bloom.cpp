#include "bloom.h"
#include "sstable.h"
#include <cmath>
#include <algorithm>
#include <cassert>
#include <numeric>

namespace cascade {

BlockedBloomFilter::BlockedBloomFilter(int total_bits, int k)
    : k_(k), n_(0) {
    // Round up to nearest full block
    int nb = std::max(1, (total_bits + BLOCK_BITS - 1) / BLOCK_BITS);
    blocks_.assign(nb * BLOCK_BYTES, 0);
}

void BlockedBloomFilter::probe(Key key, int& block_idx, uint16_t bits[]) const {
    uint64_t h_block = hash64(key);
    uint64_t h1      = hash64b(key);
    uint64_t h2      = hash64(key ^ 0x517cc1b727220a95ULL) | 1ULL; // Ensure odd step (co-prime to 512)
    int nb = numBlocks();
    // Pick block — use h_block % nb (ensures single cache-line access per probe)
    block_idx = static_cast<int>(h_block % static_cast<uint64_t>(nb));
    // Derive k bit positions within the 512-bit block using enhanced double hashing
    for (int i = 0; i < k_; i++) {
        bits[i] = static_cast<uint16_t>((h1 + static_cast<uint64_t>(i) * h2) % BLOCK_BITS);
    }
}

void BlockedBloomFilter::add(Key key) {
    int block_idx;
    uint16_t bits[32]; // k <= 32
    probe(key, block_idx, bits);
    uint8_t* block = &blocks_[block_idx * BLOCK_BYTES];
    for (int i = 0; i < k_; i++) {
        block[bits[i] / 8] |= (1u << (bits[i] % 8));
    }
    n_++;
}

bool BlockedBloomFilter::possiblyContains(Key key) const {
    if (numBlocks() == 0) return false;
    int block_idx;
    uint16_t bits[32];
    probe(key, block_idx, bits);
    const uint8_t* block = &blocks_[block_idx * BLOCK_BYTES];
    for (int i = 0; i < k_; i++) {
        if (!(block[bits[i] / 8] & (1u << (bits[i] % 8))))
            return false;
    }
    return true;
}

void BlockedBloomFilter::rebuild(int new_total_bits) {
    int nb = std::max(1, (new_total_bits + BLOCK_BITS - 1) / BLOCK_BITS);
    blocks_.assign(nb * BLOCK_BYTES, 0);
    n_ = 0;
}

void BlockedBloomFilter::clear() {
    std::fill(blocks_.begin(), blocks_.end(), 0);
    n_ = 0;
}

double BlockedBloomFilter::fpr() const {
    if (n_ == 0) return 0.0;
    int m = totalBits();
    double exponent = -static_cast<double>(k_) * n_ / m;
    return std::pow(1.0 - std::exp(exponent), k_);
}

std::vector<uint8_t> BlockedBloomFilter::serialize() const {
    std::vector<uint8_t> buf(12 + blocks_.size());
    uint32_t k_u = k_;
    uint32_t n_u = n_;
    uint32_t nb_u = numBlocks();
    std::memcpy(buf.data(), &k_u, 4);
    std::memcpy(buf.data() + 4, &n_u, 4);
    std::memcpy(buf.data() + 8, &nb_u, 4);
    if (!blocks_.empty()) {
        std::memcpy(buf.data() + 12, blocks_.data(), blocks_.size());
    }
    return buf;
}

BlockedBloomFilter BlockedBloomFilter::deserialize(const uint8_t* data, size_t size) {
    if (size < 12) return BlockedBloomFilter(512, 8);
    uint32_t k_u = 0, n_u = 0, nb_u = 0;
    std::memcpy(&k_u, data, 4);
    std::memcpy(&n_u, data + 4, 4);
    std::memcpy(&nb_u, data + 8, 4);
    BlockedBloomFilter bf;
    bf.k_ = k_u;
    bf.n_ = n_u;
    size_t expected_block_bytes = nb_u * BLOCK_BYTES;
    if (size >= 12 + expected_block_bytes) {
        bf.blocks_.assign(data + 12, data + 12 + expected_block_bytes);
    }
    return bf;
}

// ---------------------------------------------------------------------------
// Structural reallocation — Monkey-optimal per-level sizing
//
// bits_i = max(512,  bits_per_key * |L_i| * depth_mult_i)
// depth_mult_i = 1 + 0.1 * i   (deeper levels get slightly more bits/key
//                               because false positives there cost more I/O)
// Total bits across all levels is capped at max_total_bytes * 8.
// k (hash functions) is derived via k_opt = round(ln2 * bits_per_key).
// ---------------------------------------------------------------------------
void BloomAllocator::reallocateStructural(
    std::vector<BlockedBloomFilter>& filters,
    const std::vector<std::vector<std::vector<KVPair>>>& levels,
    int bits_per_key,
    size_t max_total_bytes)
{
    int L = (int)levels.size();
    while ((int)filters.size() < L)
        filters.emplace_back(512, 8);

    // Count keys per level
    std::vector<int> counts(L, 0);
    for (int i = 0; i < L; i++)
        for (auto& run : levels[i])
            counts[i] += (int)run.size();

    // Compute raw bit allocation per level (before cap)
    const int64_t max_total_bits = (int64_t)max_total_bytes * 8;
    std::vector<int64_t> raw_bits(L, 512);
    int64_t total_raw = 0;
    for (int i = 0; i < L; i++) {
        double depth_mult = 1.0 + 0.1 * i; // L0=1.0, L1=1.1, ..., L6=1.6
        raw_bits[i] = std::max(
            (int64_t)512,
            (int64_t)(bits_per_key * counts[i] * depth_mult)
        );
        total_raw += raw_bits[i];
    }

    // Apply safety cap: scale down proportionally if over budget
    double scale = (total_raw > max_total_bits && total_raw > 0)
        ? (double)max_total_bits / total_raw
        : 1.0;

    // Optimal k from effective bits/key (may differ per level after scaling)
    int k_base = optimalBloomK(bits_per_key); // e.g. k=7 for 10 bits/key

    for (int i = 0; i < L; i++) {
        int bits = std::max(512, (int)(raw_bits[i] * scale));
        filters[i].rebuild(bits);
        for (auto& run : levels[i])
            for (auto& kv : run)
                filters[i].add(kv.key); // tombstones stay in bloom
    }
    (void)k_base; // k is baked into BlockedBloomFilter at construction; rebuild() preserves k_
}

// ---------------------------------------------------------------------------
// Frequency trigger — Merlin-style hot-level bloom boost
//
// Hot levels (high access_tracker.hottest()) receive an extra
// +10% bits/key on top of their current allocation.
// This is applied as an additive expansion: rebuild with more bits and re-add.
// ---------------------------------------------------------------------------
void BloomAllocator::reallocateFrequency(
    std::vector<BlockedBloomFilter>& filters,
    const std::vector<SStableAccessTracker>& trackers,
    const std::vector<std::vector<std::vector<KVPair>>>& levels,
    int bits_per_key)
{
    int L = std::min({(int)filters.size(), (int)trackers.size(), (int)levels.size()});
    if (L == 0) return;

    std::vector<int> hot(L, 0);
    int hot_total = 0;
    for (int i = 0; i < L; i++) {
        hot[i] = trackers[i].hottest();
        hot_total += hot[i];
    }
    if (hot_total == 0) return;

    // Extra bits/key proportional to hotness share (capped at +10% of base)
    int bonus_per_key = std::max(1, bits_per_key / 10);
    for (int i = 0; i < L; i++) {
        if (hot[i] == 0) continue;
        // Extra bits = bonus_per_key * count_i (hot level gets full bonus)
        int count_i = 0;
        for (auto& run : levels[i]) count_i += (int)run.size();
        int extra_bits = (int)((double)hot[i] / hot_total * bonus_per_key * count_i);
        if (extra_bits <= 0) continue;
        int new_bits = filters[i].totalBits() + extra_bits;
        filters[i].rebuild(new_bits);
        for (auto& run : levels[i])
            for (auto& kv : run)
                filters[i].add(kv.key);
    }
}

void BloomAllocator::reallocateStructural(
    std::vector<BlockedBloomFilter>& filters,
    const std::vector<std::vector<std::shared_ptr<SSTable>>>& levels,
    int bits_per_key,
    size_t max_total_bytes)
{
    int L = (int)levels.size();
    while ((int)filters.size() < L)
        filters.emplace_back(512, 8);

    std::vector<int> counts(L, 0);
    for (int i = 0; i < L; i++)
        for (auto& sst : levels[i])
            if (sst) counts[i] += sst->numEntries();

    const int64_t max_total_bits = (int64_t)max_total_bytes * 8;
    std::vector<int64_t> raw_bits(L, 512);
    int64_t total_raw = 0;
    for (int i = 0; i < L; i++) {
        double depth_mult = 1.0 + 0.1 * i;
        raw_bits[i] = std::max(
            (int64_t)512,
            (int64_t)(bits_per_key * counts[i] * depth_mult)
        );
        total_raw += raw_bits[i];
    }

    double scale = (total_raw > max_total_bits && total_raw > 0)
        ? (double)max_total_bits / total_raw
        : 1.0;

    for (int i = 0; i < L; i++) {
        int bits = std::max(512, (int)(raw_bits[i] * scale));
        filters[i].rebuild(bits);
        for (auto& sst : levels[i]) {
            if (!sst) continue;
            auto pairs = sst->readAll();
            for (auto& kv : pairs)
                filters[i].add(kv.key);
        }
    }
}

void BloomAllocator::reallocateFrequency(
    std::vector<BlockedBloomFilter>& filters,
    const std::vector<SStableAccessTracker>& trackers,
    const std::vector<std::vector<std::shared_ptr<SSTable>>>& levels,
    int bits_per_key)
{
    int L = std::min({(int)filters.size(), (int)trackers.size(), (int)levels.size()});
    if (L == 0) return;

    std::vector<int> hot(L, 0);
    int hot_total = 0;
    for (int i = 0; i < L; i++) {
        hot[i] = trackers[i].hottest();
        hot_total += hot[i];
    }
    if (hot_total == 0) return;

    int bonus_per_key = std::max(1, bits_per_key / 10);
    for (int i = 0; i < L; i++) {
        if (hot[i] == 0) continue;
        int count_i = 0;
        for (auto& sst : levels[i])
            if (sst) count_i += sst->numEntries();
        int extra_bits = (int)((double)hot[i] / hot_total * bonus_per_key * count_i);
        if (extra_bits <= 0) continue;
        int new_bits = filters[i].totalBits() + extra_bits;
        filters[i].rebuild(new_bits);
        for (auto& sst : levels[i]) {
            if (!sst) continue;
            auto pairs = sst->readAll();
            for (auto& kv : pairs)
                filters[i].add(kv.key);
        }
    }
}

} // namespace cascade
