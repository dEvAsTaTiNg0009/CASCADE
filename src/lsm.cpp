#include "lsm.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <cassert>
#include <numeric>
#include <iomanip>

using namespace std::chrono;

namespace cascade {

LSMEngine::LSMEngine(const Config& cfg)
    : cfg_(cfg), ahlc_(cfg), wal_(cfg.wal_group_commit_batch)
{
    levels_.resize(cfg.max_levels);
    bloom_filters_.reserve(cfg.max_levels);
    access_trackers_.resize(cfg.max_levels);
    int bits_per_level = cfg.bloom_total_budget / cfg.max_levels;
    for (int i = 0; i < cfg.max_levels; i++)
        bloom_filters_.emplace_back(bits_per_level, 8);

    block_cache_ = std::make_unique<BlockCache>(cfg.block_cache_capacity);

    // Start background compaction thread
    compact_thread_ = std::thread([this]{ backgroundLoop(); });
}

LSMEngine::~LSMEngine() {
    stop_bg_.store(true, std::memory_order_release);
    compact_cv_.notify_all();
    if (compact_thread_.joinable()) compact_thread_.join();
    EpochManager::instance().runGC();
}

// ---------------------------------------------------------------------------
// Background compaction loop — woken up by requestCompaction()
// ---------------------------------------------------------------------------
void LSMEngine::backgroundLoop() {
    while (true) {
        std::unique_lock<std::mutex> lk(compact_mu_);
        compact_cv_.wait(lk, [this]{
            return compact_requested_.load() || stop_bg_.load();
        });
        if (stop_bg_.load()) break;
        compact_requested_.store(false);
        lk.unlock();

        doCompaction();
        EpochManager::instance().runGC();
    }
}

void LSMEngine::requestCompaction() {
    compact_requested_.store(true, std::memory_order_release);
    compact_cv_.notify_one();
}

// ---------------------------------------------------------------------------
// INSERT
// ---------------------------------------------------------------------------
void LSMEngine::insert(Key key, const Value& value) {
    auto t0 = high_resolution_clock::now();

    // WAL first (group commit)
    wal_.append(WalRecordType::PUT, key, value);
    metrics_.bytes_written_wal.fetch_add(
        (int64_t)(8 + 4 + value.size()), std::memory_order_relaxed);
    metrics_.bytes_ingested_logical.fetch_add(
        cfg_.bytes_per_kv, std::memory_order_relaxed);

    // Insert into concurrent MemTable
    memtable_.insert(key, value);
    metrics_.total_inserts.fetch_add(1, std::memory_order_relaxed);
    metrics_.logical_live_keys.fetch_add(1, std::memory_order_relaxed);

    // Flush if full
    if (memtable_.isFull(cfg_.memtable_capacity)) {
        flush();
    }

    auto t1 = high_resolution_clock::now();
    metrics_.write_latency.record(
        duration_cast<nanoseconds>(t1 - t0).count());
}

// ---------------------------------------------------------------------------
// DELETE — tombstone insert
// ---------------------------------------------------------------------------
void LSMEngine::del(Key key) {
    wal_.append(WalRecordType::DELETE, key);
    memtable_.del(key);
    metrics_.total_deletes.fetch_add(1, std::memory_order_relaxed);
    metrics_.logical_live_keys.fetch_add(-1, std::memory_order_relaxed);
    if (memtable_.isFull(cfg_.memtable_capacity)) flush();
}

// ---------------------------------------------------------------------------
// SEARCH — MemTable → L0 → ... → Ln, bloom-gated
// ---------------------------------------------------------------------------
bool LSMEngine::search(Key key, Value& out) {
    auto t0 = high_resolution_clock::now();
    metrics_.total_reads.fetch_add(1, std::memory_order_relaxed);
    skew_.recordAccess(key);

    // 1. MemTable (lock-free OLC search)
    bool is_tombstone = false;
    if (memtable_.search(key, out, is_tombstone)) {
        if (is_tombstone) {
            auto t1 = high_resolution_clock::now();
            metrics_.read_latency.record(duration_cast<nanoseconds>(t1-t0).count());
            return false;
        }
        metrics_.read_hits.fetch_add(1, std::memory_order_relaxed);
        auto t1 = high_resolution_clock::now();
        metrics_.read_latency.record(duration_cast<nanoseconds>(t1-t0).count());
        return true;
    }

    // 2. Level scan — newest level first
    std::lock_guard<std::mutex> lk(levels_mu_);
    for (int i = 0; i < (int)levels_.size(); i++) {
        if (levels_[i].empty()) continue;
        if ((int)bloom_filters_.size() <= i) continue;

        metrics_.bloom_probes.fetch_add(1, std::memory_order_relaxed);
        access_trackers_[i].recordAccess((int)i);

        if (!bloom_filters_[i].possiblyContains(key)) continue; // true negative

        // Bloom says maybe — search runs newest first
        bool found = false;
        for (int j = (int)levels_[i].size() - 1; j >= 0; j--) {
            auto& run = levels_[i][j];
            metrics_.sstable_block_reads.fetch_add(1, std::memory_order_relaxed);
            auto it = std::lower_bound(run.begin(), run.end(), KVPair{key},
                [](const KVPair& a, const KVPair& b){ return a.key < b.key; });
            if (it != run.end() && it->key == key) {
                if (it->is_tombstone) {
                    auto t1 = high_resolution_clock::now();
                    metrics_.read_latency.record(duration_cast<nanoseconds>(t1-t0).count());
                    return false;
                }
                out = it->value;
                metrics_.read_hits.fetch_add(1, std::memory_order_relaxed);
                found = true;
                break;
            }
        }
        if (found) {
            auto t1 = high_resolution_clock::now();
            metrics_.read_latency.record(duration_cast<nanoseconds>(t1-t0).count());
            return true;
        }
        metrics_.bloom_false_positives.fetch_add(1, std::memory_order_relaxed);
    }

    auto t1 = high_resolution_clock::now();
    metrics_.read_latency.record(duration_cast<nanoseconds>(t1-t0).count());
    return false;
}

// ---------------------------------------------------------------------------
// SCAN
// ---------------------------------------------------------------------------
std::vector<KVPair> LSMEngine::scan(Key start, Key end) {
    auto t0 = high_resolution_clock::now();

    std::vector<KVPair> result;

    // MemTable scan
    auto mt_result = memtable_.scan(start, end);
    result.insert(result.end(), mt_result.begin(), mt_result.end());

    // Level scan
    {
        std::lock_guard<std::mutex> lk(levels_mu_);
        for (int i = 0; i < (int)levels_.size(); i++) {
            for (auto& run : levels_[i]) {
                auto it = std::lower_bound(run.begin(), run.end(), KVPair{start},
                    [](const KVPair& a, const KVPair& b){ return a.key < b.key; });
                while (it != run.end() && it->key <= end) {
                    result.push_back(*it++);
                }
            }
        }
    }

    // Merge dedup (newest wins)
    std::stable_sort(result.begin(), result.end());
    std::vector<KVPair> deduped;
    for (auto& kv : result) {
        if (deduped.empty() || deduped.back().key != kv.key)
            if (!kv.is_tombstone) deduped.push_back(kv);
    }

    auto t1 = high_resolution_clock::now();
    metrics_.scan_latency.record(duration_cast<nanoseconds>(t1-t0).count());
    return deduped;
}

// ---------------------------------------------------------------------------
// FLUSH — MemTable → L0
// ---------------------------------------------------------------------------
void LSMEngine::flush() {
    auto sorted = memtable_.flush();
    if (sorted.empty()) return;
    doFlush(std::move(sorted));
    requestCompaction();
}

void LSMEngine::doFlush(std::vector<KVPair> sorted) {
    int bytes_flushed = (int)sorted.size() * cfg_.bytes_per_kv;

    {
        std::lock_guard<std::mutex> lk(levels_mu_);
        levels_[0].push_back(std::move(sorted));
        // Add to L0 bloom
        if (!bloom_filters_.empty()) {
            for (auto& kv : levels_[0].back())
                bloom_filters_[0].add(kv.key); // tombstones must be in bloom to intercept older versions
        }
    }

    metrics_.bytes_written_sstables.fetch_add(bytes_flushed, std::memory_order_relaxed);
    bytes_written_.fetch_add(bytes_flushed, std::memory_order_relaxed);
    metrics_.total_flushes.fetch_add(1, std::memory_order_relaxed);

    velocity_.recordFlush(bytes_flushed);
    wal_.sync();
}

// ---------------------------------------------------------------------------
// COMPACTION — AHLC-directed
// ---------------------------------------------------------------------------
void LSMEngine::doCompaction() {
    double vw   = velocity_.velocity();
    double skew = skew_.gini();

    bool any_full = false;
    for (int i = 0; i < (int)levels_.size(); i++)
        if (isLevelFull(i)) { any_full = true; break; }

    Strategy s = ahlc_.evaluate(vw, skew, any_full);
    if (ahlc_.switches() > 0)
        metrics_.strategy_switches.fetch_add(
            (ahlc_.switches() > metrics_.strategy_switches.load()) ? 1 : 0,
            std::memory_order_relaxed);

    std::lock_guard<std::mutex> lk(levels_mu_);
    auto params = ahlc_.levelParams();

    for (int i = 0; i < (int)levels_.size() - 1; i++) {
        if (!isLevelFull(i)) continue;

        switch (s) {
            case Strategy::LEVELING:
                compactLeveling(levels_, i, bytes_written_);
                break;
            case Strategy::TIERING:
                compactTiering(levels_, i, params.maxRuns, bytes_written_);
                break;
            case Strategy::HYBRID:
                if (i >= (int)levels_.size() - 2)
                    compactLeveling(levels_, i, bytes_written_);
                else
                    compactTiering(levels_, i, params.maxRuns, bytes_written_);
                break;
        }
        metrics_.total_compactions.fetch_add(1, std::memory_order_relaxed);
        metrics_.bytes_written_sstables.store(bytes_written_.load(), std::memory_order_relaxed);
        break; // one level per compaction round
    }

    // Rebuild blooms atomically with level changes
    rebuildBlooms();

    // Update physical key count
    metrics_.physical_keys_on_disk.store(countPhysicalKeys(), std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// BLOOM REBUILD — dual-trigger
// ---------------------------------------------------------------------------
void LSMEngine::rebuildBlooms() {
    // Structural trigger: called after compaction
    BloomAllocator::reallocateStructural(bloom_filters_, levels_, cfg_.bloom_total_budget);

    // Frequency trigger: micro-boost hot levels
    if ((int)access_trackers_.size() >= (int)bloom_filters_.size())
        BloomAllocator::reallocateFrequency(bloom_filters_, access_trackers_,
                                             levels_, cfg_.bloom_total_budget);
}

// ---------------------------------------------------------------------------
// HELPERS
// ---------------------------------------------------------------------------
bool LSMEngine::isLevelFull(int level) const {
    if (level < 0 || level >= (int)levels_.size()) return false;
    int total = 0;
    for (auto& r : levels_[level]) total += (int)r.size();
    auto params = ahlc_.levelParams();
    int cap = cfg_.memtable_capacity * (level + 1) * params.capacityMult;
    return total > cap || (int)levels_[level].size() > params.maxRuns;
}

int LSMEngine::countPhysicalKeys() const {
    int total = 0;
    for (auto& level : levels_)
        for (auto& run : level)
            total += (int)run.size();
    return total;
}

void LSMEngine::printMetrics(const std::string& label, double elapsed_sec) {
    metrics_.physical_keys_on_disk.store(countPhysicalKeys(), std::memory_order_relaxed);
    metrics_.print(label, elapsed_sec);
}

} // namespace cascade
