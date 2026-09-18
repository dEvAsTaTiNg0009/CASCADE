#include "bloom.h"
#include "sstable.h"
#include <cmath>
#include <algorithm>
#include <cassert>
#include <numeric>
#include <cstdio>  // fprintf

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
    // NOTE: k_ is NOT changed by this overload.  Use rebuild(bits, k) to also
    // update the hash function count (e.g. after reallocateStructural).
}

// New overload: resize AND update k simultaneously.
// Called by reallocateStructural/Frequency so the optimal k is applied
// consistently with the new bit budget.
void BlockedBloomFilter::rebuild(int new_total_bits, int new_k) {
    k_ = std::max(1, std::min(new_k, 32)); // clamp to [1,32] (bits array limit)
    rebuild(new_total_bits); // calls the single-arg overload which resets blocks_ and n_
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
// allocateWithBudget — CENTRALIZED budget-preserving allocator.
//
// Algorithm (STEP 2A + 2B):
//   1. Compute normalized weights: w_i = f_i * d_i (with optional freq_factor_i)
//      where f_i = count_i / sum_j count_j, d_i = 1 + 0.1*i.
//      Normalize: w_i /= W = sum_j w_j.
//   2. Pre-assign b_min = 512 bits per non-empty level (water-filling floor).
//   3. Distribute remaining budget B' = total_budget_bits - L*b_min
//      proportional to w_i.
//   4. Final b_i = b_min + floor(B' * w_i).  Round up remainder to the level
//      with the largest fractional part.
//   5. If L * b_min > total_budget_bits, emit a warning and fall back to
//      b_min for every level (cannot conserve budget — caller must handle).
//
// Guarantees: every b_i >= b_min, sum(b_i) <= total_budget_bits.
// When total_budget_bits is an exact multiple of the block size the allocation
// is within 1 block (512 bits) of budget.
// ---------------------------------------------------------------------------
std::vector<int64_t> BloomAllocator::allocateWithBudget(
    const std::vector<int>& counts,
    int64_t total_budget_bits,
    int /*bits_per_key*/,
    const std::vector<double>& freq_factors)
{
    int L = (int)counts.size();
    static constexpr int64_t B_MIN = 512; // one Bloom block = minimum allocation

    std::vector<int64_t> alloc(L, B_MIN);

    // Edge-case: budget can't even cover the minimum floor for all levels.
    if ((int64_t)L * B_MIN > total_budget_bits) {
        fprintf(stderr,
            "[Bloom] WARNING: budget %lld bits < L=%d × b_min=%lld. "
            "Cannot conserve global budget; each level gets b_min only.\n",
            (long long)total_budget_bits, L, (long long)B_MIN);
        return alloc; // sum = L * B_MIN, which may exceed the budget
    }

    // Allocate in whole blocks of 512 bits directly to ensure exact block alignment
    // and strict budget preservation: sum(alloc) <= total_budget_bits.
    int64_t total_blocks = total_budget_bits / B_MIN;
    int64_t remaining_blocks = total_blocks - L; // pre-assign 1 block (512 bits) to each level
    std::vector<int64_t> block_alloc(L, 1);

    // Compute total entry count for frequency normalization.
    int64_t total_count = 0;
    for (int c : counts) total_count += c;

    // Compute unnormalized weights w_i = f_i * d_i * freq_factor_i.
    // For empty levels (count==0), w_i = 0 (they keep only b_min).
    std::vector<double> weights(L, 0.0);
    double W = 0.0;
    for (int i = 0; i < L; i++) {
        if (counts[i] <= 0) continue;
        double f_i = (total_count > 0) ? (double)counts[i] / total_count : 0.0;
        double d_i = 1.0 + 0.1 * i;
        double fr  = (i < (int)freq_factors.size() && freq_factors[i] > 0.0)
                     ? freq_factors[i] : 1.0;
        weights[i] = f_i * d_i * fr;
        W += weights[i];
    }

    if (W > 0.0 && remaining_blocks > 0) {
        std::vector<double> ideal(L, 0.0);
        int64_t allocated_extra = 0;
        for (int i = 0; i < L; i++) {
            ideal[i] = (double)remaining_blocks * (weights[i] / W);
            int64_t extra = static_cast<int64_t>(ideal[i]);
            block_alloc[i] += extra;
            allocated_extra += extra;
        }

        int64_t leftover_blocks = remaining_blocks - allocated_extra;
        if (leftover_blocks > 0) {
            std::vector<std::pair<double, int>> fractions;
            fractions.reserve(L);
            for (int i = 0; i < L; i++) {
                fractions.push_back({ideal[i] - std::floor(ideal[i]), i});
            }
            std::sort(fractions.begin(), fractions.end(),
                      [](const auto& a, const auto& b){ return a.first > b.first; });
            for (int k = 0; k < (int)leftover_blocks && k < L; k++) {
                block_alloc[fractions[k].second]++;
            }
        }
    }

    for (int i = 0; i < L; i++) {
        alloc[i] = block_alloc[i] * B_MIN;
    }

    return alloc;
}

// ---------------------------------------------------------------------------
// Structural reallocation — Monkey-optimal per-level sizing.
//
// Uses allocateWithBudget() for guaranteed budget conservation.
// Also updates k_ on each filter to the optimal value for bits_per_key.
// ---------------------------------------------------------------------------
void BloomAllocator::reallocateStructural(
    std::vector<BlockedBloomFilter>& filters,
    const std::vector<std::vector<std::vector<KVPair>>>& levels,
    int bits_per_key,
    size_t max_total_bytes)
{
    int L = (int)levels.size();
    while ((int)filters.size() < L)
        filters.emplace_back(512, optimalBloomK(bits_per_key));

    // Count keys per level
    std::vector<int> counts(L, 0);
    int64_t total_count = 0;
    for (int i = 0; i < L; i++) {
        for (auto& run : levels[i])
            counts[i] += (int)run.size();
        total_count += counts[i];
    }

    int64_t max_total_bits = (int64_t)max_total_bytes * 8;
    int64_t target_budget  = std::min((int64_t)total_count * bits_per_key, max_total_bits);
    target_budget = std::max(target_budget, (int64_t)L * 512);

    auto alloc = allocateWithBudget(counts, target_budget, bits_per_key);

    // Optimal k from configured bits_per_key — applied uniformly since the
    // per-level bits/key ratio is close to bits_per_key by construction.
    //
    // INVARIANT: k_opt MUST always come from optimalBloomK(bits_per_key).
    // Do NOT replace this with a hardcoded constant or a stale cached value.
    // rebuild(bits, k_opt) below sets k_ on the live filter; if k diverges from
    // optimalBloomK() the FPR model in BlockedBloomFilter::fpr() and all
    // paper-reported FPR numbers will be incorrect.  LSMEngine::verifyBloomKInvariant()
    // asserts this post-condition after every rebuildBlooms() call in the test suite.
    int k_opt = optimalBloomK(bits_per_key);

    for (int i = 0; i < L; i++) {
        int bits = (int)alloc[i];
        // rebuild(bits, k_opt): clears the filter AND sets k_ = optimalBloomK().
        // This is the single authoritative place where k_ is written after initial
        // construction — rebuild() never touches k_ unless the two-arg overload
        // is used, so a future refactor must keep this two-arg form.
        filters[i].rebuild(bits, k_opt);
        for (auto& run : levels[i])
            for (auto& kv : run)
                filters[i].add(kv.key); // tombstones stay in bloom
    }
}

// ---------------------------------------------------------------------------
// Frequency trigger — Merlin-style hot-level bloom boost.
//
// Frequency information modifies the allocation WEIGHTS used by
// allocateWithBudget(), NOT the bits directly.  This preserves the global
// budget regardless of the frequency distribution.
//
// freq_factor_i = 1.0 + hotness_share_i * bonus_fraction
// where bonus_fraction = 0.10 (10% maximum bonus for the hottest level).
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

    // Build per-level frequency factors: 1.0 + hotness_share * 0.10
    static constexpr double BONUS_FRACTION = 0.10;
    std::vector<double> freq_factors(L, 1.0);
    for (int i = 0; i < L; i++) {
        freq_factors[i] = 1.0 + ((double)hot[i] / hot_total) * BONUS_FRACTION;
    }

    std::vector<int> counts(L, 0);
    for (int i = 0; i < L; i++)
        for (auto& run : levels[i]) counts[i] += (int)run.size();

    // Total budget: sum of current filter allocations (treat as fixed).
    // We re-distribute the SAME total budget with frequency-weighted weights.
    int64_t current_total = 0;
    for (int i = 0; i < L; i++) current_total += filters[i].totalBits();
    if (current_total <= 0) return;

    auto alloc = allocateWithBudget(counts, current_total, bits_per_key, freq_factors);
    int k_opt  = optimalBloomK(bits_per_key);

    for (int i = 0; i < L; i++) {
        filters[i].rebuild((int)alloc[i], k_opt);
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
        filters.emplace_back(512, optimalBloomK(bits_per_key));

    std::vector<int> counts(L, 0);
    int64_t total_count = 0;
    for (int i = 0; i < L; i++) {
        for (auto& sst : levels[i])
            if (sst) counts[i] += sst->numEntries();
        total_count += counts[i];
    }

    int64_t max_total_bits = (int64_t)max_total_bytes * 8;
    int64_t target_budget  = std::min((int64_t)total_count * bits_per_key, max_total_bits);
    target_budget = std::max(target_budget, (int64_t)L * 512);

    auto alloc = allocateWithBudget(counts, target_budget, bits_per_key);
    // INVARIANT: k_opt MUST always come from optimalBloomK(bits_per_key).
    // See the KVPair overload above for the full rationale.
    int k_opt  = optimalBloomK(bits_per_key);

    for (int i = 0; i < L; i++) {
        filters[i].rebuild((int)alloc[i], k_opt);
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

    static constexpr double BONUS_FRACTION = 0.10;
    std::vector<double> freq_factors(L, 1.0);
    for (int i = 0; i < L; i++) {
        freq_factors[i] = 1.0 + ((double)hot[i] / hot_total) * BONUS_FRACTION;
    }

    std::vector<int> counts(L, 0);
    for (int i = 0; i < L; i++)
        for (auto& sst : levels[i])
            if (sst) counts[i] += sst->numEntries();

    int64_t current_total = 0;
    for (int i = 0; i < L; i++) current_total += filters[i].totalBits();
    if (current_total <= 0) return;

    auto alloc = allocateWithBudget(counts, current_total, bits_per_key, freq_factors);
    int k_opt  = optimalBloomK(bits_per_key);

    for (int i = 0; i < L; i++) {
        filters[i].rebuild((int)alloc[i], k_opt);
        for (auto& sst : levels[i]) {
            if (!sst) continue;
            auto pairs = sst->readAll();
            for (auto& kv : pairs)
                filters[i].add(kv.key);
        }
    }
}

} // namespace cascade
