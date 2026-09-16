#include "lsm.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <cassert>
#include <numeric>
#include <iomanip>
#include <dirent.h>
#include <sys/stat.h>

using namespace std::chrono;

namespace cascade {

LSMEngine::LSMEngine(const Config& cfg)
    : cfg_(cfg), ahlc_(cfg), wal_(cfg.wal_group_commit_batch)
{
    // Ensure database directory exists
    struct stat st;
    if (::stat(cfg_.db_path.c_str(), &st) != 0) {
        ::mkdir(cfg_.db_path.c_str(), 0755);
    }

    levels_.resize(cfg.max_levels);
    bloom_filters_.reserve(cfg.max_levels);
    access_trackers_.resize(cfg.max_levels);
    for (int i = 0; i < cfg.max_levels; i++)
        bloom_filters_.emplace_back(512, optimalBloomK(cfg.bloom_bits_per_key));

    block_cache_ = std::make_unique<BlockCache>(cfg.block_cache_capacity);

    // Select memtable implementation
    if (cfg_.memtable_type == MemtableType::SKIP_LIST)
        skiplist_mt_ = std::make_unique<SkipListMemtable>();

    // Open real WAL file in database directory
    wal_.open(cfg_.db_path + "/wal.log", cfg_.wal_group_commit_batch);

    // Load any existing SSTable files on disk
    loadExistingSSTables();

    // Replay WAL records if recovering from crash or non-flushed state
    auto wal_records = wal_.recover();
    for (const auto& rec : wal_records) {
        if (rec.type == WalRecordType::PUT) {
            if (cfg_.memtable_type == MemtableType::SKIP_LIST)
                skiplist_mt_->insert(rec.key, rec.value);
            else
                memtable_.insert(rec.key, rec.value);
            metrics_.logical_live_keys.fetch_add(1, std::memory_order_relaxed);
        } else if (rec.type == WalRecordType::DELETE) {
            if (cfg_.memtable_type == MemtableType::SKIP_LIST)
                skiplist_mt_->del(rec.key);
            else
                memtable_.del(rec.key);
            metrics_.logical_live_keys.fetch_add(-1, std::memory_order_relaxed);
        }
    }

    // Start background compaction thread
    compact_thread_ = std::thread([this]{ backgroundLoop(); });
}

LSMEngine::~LSMEngine() {
    stop_bg_.store(true, std::memory_order_release);
    compact_cv_.notify_all();
    if (compact_thread_.joinable()) compact_thread_.join();
    EpochManager::instance().runGC();
}

void LSMEngine::loadExistingSSTables() {
    DIR* dir = opendir(cfg_.db_path.c_str());
    if (!dir) return;

    struct dirent* entry;
    std::vector<std::pair<int, std::pair<uint64_t, std::string>>> found;

    while ((entry = readdir(dir)) != nullptr) {
        std::string fname = entry->d_name;
        if (fname.size() > 5 && fname[0] == 'L' && fname.substr(fname.size() - 4) == ".sst") {
            size_t underscore = fname.find('_');
            if (underscore != std::string::npos && underscore > 1) {
                try {
                    int lvl = std::stoi(fname.substr(1, underscore - 1));
                    uint64_t id = std::stoull(fname.substr(underscore + 1, fname.size() - underscore - 5));
                    found.push_back({lvl, {id, cfg_.db_path + "/" + fname}});
                } catch (...) {}
            }
        }
    }
    closedir(dir);

    std::sort(found.begin(), found.end());

    std::lock_guard<std::mutex> lk(levels_mu_);
    for (auto& item : found) {
        int lvl = item.first;
        uint64_t id = item.second.first;
        const std::string& path = item.second.second;

        while ((int)levels_.size() <= lvl) {
            levels_.push_back({});
        }

        auto sst = SSTable::open(id, path);
        if (sst) {
            levels_[lvl].push_back(sst);
            if (id >= SSTable::next_id_) {
                SSTable::next_id_ = id + 1;
            }
        }
    }

    if (!found.empty()) {
        rebuildBlooms();
        metrics_.physical_keys_on_disk.store(countPhysicalKeys(), std::memory_order_relaxed);
    }
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
    int64_t kv_bytes = (int64_t)(8 + 4 + value.size());
    metrics_.bytes_written_wal.fetch_add(kv_bytes, std::memory_order_relaxed);
    metrics_.bytes_ingested_logical.fetch_add(kv_bytes, std::memory_order_relaxed);

    // Insert into active memtable
    if (cfg_.memtable_type == MemtableType::SKIP_LIST)
        skiplist_mt_->insert(key, value);
    else
        memtable_.insert(key, value);
    metrics_.total_inserts.fetch_add(1, std::memory_order_relaxed);
    metrics_.logical_live_keys.fetch_add(1, std::memory_order_relaxed);

    // Flush if full
    bool full = (cfg_.memtable_type == MemtableType::SKIP_LIST)
        ? skiplist_mt_->isFull(cfg_.memtable_capacity)
        : memtable_.isFull(cfg_.memtable_capacity);
    if (full) flush();

    auto t1 = high_resolution_clock::now();
    metrics_.write_latency.record(
        duration_cast<nanoseconds>(t1 - t0).count());
}

// ---------------------------------------------------------------------------
// DELETE — tombstone insert
// ---------------------------------------------------------------------------
void LSMEngine::del(Key key) {
    int64_t tomb_bytes = (int64_t)(8 + 4);
    wal_.append(WalRecordType::DELETE, key);
    metrics_.bytes_written_wal.fetch_add(tomb_bytes, std::memory_order_relaxed);
    metrics_.bytes_ingested_logical.fetch_add(tomb_bytes, std::memory_order_relaxed);

    if (cfg_.memtable_type == MemtableType::SKIP_LIST)
        skiplist_mt_->del(key);
    else
        memtable_.del(key);
    metrics_.total_deletes.fetch_add(1, std::memory_order_relaxed);
    metrics_.logical_live_keys.fetch_add(-1, std::memory_order_relaxed);
    bool full = (cfg_.memtable_type == MemtableType::SKIP_LIST)
        ? skiplist_mt_->isFull(cfg_.memtable_capacity)
        : memtable_.isFull(cfg_.memtable_capacity);
    if (full) flush();
}

// ---------------------------------------------------------------------------
// SEARCH — MemTable → L0 → ... → Ln, bloom-gated
// ---------------------------------------------------------------------------
bool LSMEngine::search(Key key, Value& out) {
    auto t0 = high_resolution_clock::now();
    metrics_.total_reads.fetch_add(1, std::memory_order_relaxed);
    skew_.recordAccess(key);

    // 1. MemTable (lock-free search)
    bool is_tombstone = false;
    bool found_in_mt  = false;
    if (cfg_.memtable_type == MemtableType::SKIP_LIST) {
        found_in_mt = skiplist_mt_->search(key, out, is_tombstone);
    } else {
        found_in_mt = memtable_.search(key, out, is_tombstone);
    }
    if (found_in_mt) {
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
        if (i < (int)access_trackers_.size())
            access_trackers_[i].recordAccess((int)i);

        if (!bloom_filters_[i].possiblyContains(key)) continue; // true negative

        // Bloom says maybe — search SSTables newest first
        bool found = false;
        for (int j = (int)levels_[i].size() - 1; j >= 0; j--) {
            auto& sst = levels_[i][j];
            if (!sst) continue;
            if (key < sst->min_key || key > sst->max_key) continue;

            metrics_.sstable_block_reads.fetch_add(1, std::memory_order_relaxed);
            bool sst_tombstone = false;
            if (sst->search(key, out, sst_tombstone, block_cache_.get())) {
                if (sst_tombstone) {
                    auto t1 = high_resolution_clock::now();
                    metrics_.read_latency.record(duration_cast<nanoseconds>(t1-t0).count());
                    return false;
                }
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

    // MemTable scan — dispatch to the active memtable implementation
    std::vector<KVPair> mt_result;
    if (cfg_.memtable_type == MemtableType::SKIP_LIST)
        mt_result = skiplist_mt_->scan(start, end);
    else
        mt_result = memtable_.scan(start, end);
    result.insert(result.end(), mt_result.begin(), mt_result.end());

    // Level scan
    {
        std::lock_guard<std::mutex> lk(levels_mu_);
        for (int i = 0; i < (int)levels_.size(); i++) {
            for (auto& sst : levels_[i]) {
                if (!sst) continue;
                if (sst->min_key > end || sst->max_key < start) continue;
                auto sst_pairs = sst->scan(start, end, block_cache_.get());
                result.insert(result.end(), sst_pairs.begin(), sst_pairs.end());
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
    std::lock_guard<std::mutex> flk(flush_mu_);
    std::vector<KVPair> sorted;
    if (cfg_.memtable_type == MemtableType::SKIP_LIST)
        sorted = skiplist_mt_->flush();
    else
        sorted = memtable_.flush();
    if (sorted.empty()) return;
    doFlush(std::move(sorted));
    requestCompaction();
}

void LSMEngine::doFlush(std::vector<KVPair> sorted) {
    if (sorted.empty()) return;

    uint64_t id = SSTable::next_id_++;
    std::string sst_path = cfg_.db_path + "/L0_" + std::to_string(id) + ".sst";
    int bloom_bits = std::max(512, (int)(cfg_.bloom_bits_per_key * sorted.size()));
    auto sst = SSTableBuilder::build(sst_path, id, sorted, bloom_bits, metrics_.bytes_written_flush);

    int bytes_flushed = sst ? (int)sst->file_size : ((int)sorted.size() * cfg_.bytes_per_kv);

    {
        std::lock_guard<std::mutex> lk(levels_mu_);
        if (sst) {
            levels_[0].push_back(sst);
        }
        if (!bloom_filters_.empty()) {
            int l0_count = 0;
            for (auto& s : levels_[0]) if (s) l0_count += s->numEntries();
            int l0_bits = std::max(512, (int)(cfg_.bloom_bits_per_key * l0_count * 1.0));
            l0_bits = std::min(l0_bits, 32 * 1024 * 1024 * 8);
            bloom_filters_[0].rebuild(l0_bits);
            for (auto& s : levels_[0]) {
                if (!s) continue;
                auto pairs = s->readAll();
                for (auto& kv : pairs)
                    bloom_filters_[0].add(kv.key);
            }
        }
    }

    int64_t total_sst = metrics_.bytes_written_flush.load(std::memory_order_relaxed) +
                        metrics_.bytes_written_compaction.load(std::memory_order_relaxed);
    metrics_.bytes_written_sstables.store(total_sst, std::memory_order_relaxed);
    bytes_written_.store(total_sst, std::memory_order_relaxed);
    metrics_.total_flushes.fetch_add(1, std::memory_order_relaxed);

    velocity_.recordFlush(bytes_flushed);
    wal_.truncate();
}

// ---------------------------------------------------------------------------
// COMPACTION — AHLC-directed
// ---------------------------------------------------------------------------
void LSMEngine::doCompaction() {
    double vw   = velocity_.velocity();
    double skew = skew_.gini();

    std::lock_guard<std::mutex> lk(levels_mu_);
    bool any_full = false;
    for (int i = 0; i < (int)levels_.size(); i++)
        if (isLevelFull(i)) { any_full = true; break; }

    Strategy s = ahlc_.evaluate(vw, skew, any_full);
    int delta = ahlc_.drainSwitchDelta();
    if (delta > 0)
        metrics_.strategy_switches.fetch_add(delta, std::memory_order_relaxed);

    auto params = ahlc_.levelParams();

    for (int i = 0; i < (int)levels_.size(); i++) {
        if (!isLevelFull(i)) continue;

        switch (s) {
            case Strategy::LEVELING:
                compactLeveling(levels_, i, cfg_.db_path, metrics_.bytes_written_compaction, cfg_.bloom_bits_per_key);
                break;
            case Strategy::TIERING:
                compactTiering(levels_, i, params.maxRuns, cfg_.db_path, metrics_.bytes_written_compaction, cfg_.bloom_bits_per_key);
                break;
            case Strategy::HYBRID:
                if (i >= (int)levels_.size() - 1)
                    compactLeveling(levels_, i, cfg_.db_path, metrics_.bytes_written_compaction, cfg_.bloom_bits_per_key);
                else
                    compactTiering(levels_, i, params.maxRuns, cfg_.db_path, metrics_.bytes_written_compaction, cfg_.bloom_bits_per_key);
                break;
        }
        int64_t total_sst = metrics_.bytes_written_flush.load(std::memory_order_relaxed) +
                            metrics_.bytes_written_compaction.load(std::memory_order_relaxed);
        metrics_.bytes_written_sstables.store(total_sst, std::memory_order_relaxed);
        bytes_written_.store(total_sst, std::memory_order_relaxed);
        metrics_.total_compactions.fetch_add(1, std::memory_order_relaxed);
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
    while ((int)access_trackers_.size() < (int)levels_.size())
        access_trackers_.emplace_back();
    while ((int)bloom_filters_.size() < (int)levels_.size())
        bloom_filters_.emplace_back(512, 8);

    BloomAllocator::reallocateStructural(bloom_filters_, levels_,
                                         cfg_.bloom_bits_per_key,
                                         cfg_.bloom_max_bytes);

    if ((int)access_trackers_.size() >= (int)bloom_filters_.size())
        BloomAllocator::reallocateFrequency(bloom_filters_, access_trackers_,
                                            levels_, cfg_.bloom_bits_per_key);
}

// ---------------------------------------------------------------------------
// HELPERS
// ---------------------------------------------------------------------------
bool LSMEngine::isLevelFull(int level) const {
    if (level < 0 || level >= (int)levels_.size()) return false;
    int total = 0;
    for (auto& s : levels_[level]) if (s) total += s->numEntries();
    auto params = ahlc_.levelParams();
    int cap = cfg_.memtable_capacity * (level + 1) * params.capacityMult;
    return total > cap || (int)levels_[level].size() > params.maxRuns;
}

int LSMEngine::countPhysicalKeys() const {
    int total = 0;
    for (auto& level : levels_)
        for (auto& sst : level)
            if (sst) total += sst->numEntries();
    return total;
}

void LSMEngine::printMetrics(const std::string& label, double elapsed_sec) {
    metrics_.physical_keys_on_disk.store(countPhysicalKeys(), std::memory_order_relaxed);
    metrics_.print(label, elapsed_sec);
}

} // namespace cascade
