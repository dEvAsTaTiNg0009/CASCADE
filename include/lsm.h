#pragma once
// =============================================================================
// lsm.h — Cascaded LSM Engine Orchestrator
//
// Wires together all 5 modules:
//   MemTable (ConcurrentCSBTree or SkipListMemtable) → WAL → Levels →
//   AHLC Compaction → Dual-trigger Bloom → Block Cache → Metrics
// =============================================================================
#include "common.h"
#include "csb_tree.h"
#include "skiplist_mt.h"
#include "bloom.h"
#include "ahlc.h"
#include "sstable.h"
#include "wal.h"
#include "cache.h"
#include "metrics.h"
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>

namespace cascade {

class LSMEngine {
public:
    explicit LSMEngine(const Config& cfg = Config{});
    ~LSMEngine();

    // Public API
    void insert(Key key, const Value& value);
    bool search(Key key, Value& out);
    void del(Key key);
    std::vector<KVPair> scan(Key start, Key end);

    // Flush memtable manually (usually automatic)
    void flush();

    // Reporting
    EngineMetrics& metrics() { return metrics_; }
    void           printMetrics(const std::string& label, double elapsed_sec);
    AHLCEngine&    ahlc() { return ahlc_; }
    BlockCache&    blockCache() { return *block_cache_; }

    // Verifies that every Bloom filter's hash count equals optimalBloomK(cfg.bloom_bits_per_key).
    // Used by the test suite (Test 14) and for runtime regression checking.
    // Returns true iff all filters are consistent with the configured bits_per_key.
    bool verifyBloomKInvariant() const;

private:
    Config                cfg_;

    // Module 1: Concurrent MemTable — either CSB+ or SkipList (selected by cfg.memtable_type)
    ConcurrentCSBTree                  memtable_;    // used when memtable_type == CSB_PLUS
    std::unique_ptr<SkipListMemtable>  skiplist_mt_; // used when memtable_type == SKIP_LIST

    // Module 2: AHLC + telemetry
    AHLCEngine            ahlc_;
    WriteVelocityTracker  velocity_;
    GiniSkewEstimator     skew_;

    // Module 3: Bloom filter per level + access trackers
    std::vector<BlockedBloomFilter>    bloom_filters_;
    std::vector<SStableAccessTracker>  access_trackers_;

    // Module 4: Levels (list of sorted SSTables per level on disk)
    using SSTablePtr = std::shared_ptr<SSTable>;
    std::vector<std::vector<SSTablePtr>> levels_;  // levels_[i] = list of real SSTables
    mutable std::mutex                   levels_mu_;
    mutable std::mutex                   flush_mu_;

    // WAL
    WAL wal_;

    // Block cache
    std::unique_ptr<BlockCache> block_cache_;

    // Module 5: Metrics
    EngineMetrics metrics_;

    // I/O byte counters (shared with AHLC and SSTableBuilder)
    std::atomic<int64_t> bytes_written_{0};
    std::atomic<int64_t> bytes_read_{0};

    // Background compaction thread
    std::thread            compact_thread_;
    std::atomic<bool>      stop_bg_{false};
    std::condition_variable compact_cv_;
    std::mutex             compact_mu_;
    std::atomic<bool>      compact_requested_{false};

    // Internal helpers
    void doFlush(std::vector<KVPair> sorted);
    void doCompaction();
    void rebuildBlooms();
    void rebuildUniformBlooms();
    bool isLevelFull(int level) const;
    int  countPhysicalKeys() const;
    void backgroundLoop();
    void requestCompaction();
    void loadExistingSSTables();

};

} // namespace cascade
