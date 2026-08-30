#include "bloom.h"
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
    uint64_t h1 = hash64(key);
    uint64_t h2 = hash64b(key);
    int nb = numBlocks();
    // Pick block — use h1 % nb (ensures single cache-line access per probe)
    block_idx = static_cast<int>(h1 % static_cast<uint64_t>(nb));
    // Derive k bit positions within the 512-bit block using enhanced double hashing
    for (int i = 0; i < k_; i++) {
        bits[i] = static_cast<uint16_t>(nth_hash(h1, h2, i) % BLOCK_BITS);
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

// ---------------------------------------------------------------------------
// Structural reallocation: f_i = |L_i| / sum(|L_j|), d_i = 1 + 0.1*i
// b_i = max(512, floor(B * f_i*d_i / sum(f_k*d_k)))
// ---------------------------------------------------------------------------
void BloomAllocator::reallocateStructural(
    std::vector<BlockedBloomFilter>& filters,
    const std::vector<std::vector<std::vector<KVPair>>>& levels,
    int total_budget_bits)
{
    int L = (int)levels.size();
    while ((int)filters.size() < L)
        filters.emplace_back(512, 8);

    std::vector<double> counts(L, 0.0);
    for (int i = 0; i < L; i++)
        for (auto& run : levels[i])
            counts[i] += run.size();

    double total = std::accumulate(counts.begin(), counts.end(), 0.0);
    if (total == 0.0) return;

    std::vector<double> weights(L);
    double wsum = 0;
    for (int i = 0; i < L; i++) {
        double fi = counts[i] / total;
        double di = 1.0 + 0.1 * i;
        weights[i] = fi * di;
        wsum += weights[i];
    }

    for (int i = 0; i < L; i++) {
        int bits = (wsum > 0)
            ? std::max(512, (int)(total_budget_bits * weights[i] / wsum))
            : 512;
        // Rebuild filter — re-add all keys in this level
        filters[i].rebuild(bits);
        for (auto& run : levels[i])
            for (auto& kv : run)
                filters[i].add(kv.key); // add all keys (including tombstones)
    }
}

// Frequency trigger: hot level gets +10% budget, cold level yields it
void BloomAllocator::reallocateFrequency(
    std::vector<BlockedBloomFilter>& filters,
    const std::vector<SStableAccessTracker>& trackers,
    const std::vector<std::vector<std::vector<KVPair>>>& levels,
    int total_budget_bits)
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

    int bonus_budget = total_budget_bits / 10; // 10% of budget for frequency boosts
    for (int i = 0; i < L; i++) {
        int extra = (int)((double)hot[i] / hot_total * bonus_budget);
        if (extra > 0) {
            int new_bits = filters[i].totalBits() + extra;
            filters[i].rebuild(new_bits);
            for (auto& run : levels[i])
                for (auto& kv : run)
                    filters[i].add(kv.key);
        }
    }
}

} // namespace cascade
