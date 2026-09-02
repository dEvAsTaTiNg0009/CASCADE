#pragma once
// =============================================================================
// ahlc.h — Adaptive Hybrid Lightweight Compaction Engine
#include <memory>
//
// Multi-signal telemetry:
//   V_w  : EWMA write velocity (bytes/sec)
//   K_skew: Gini-index over sliding window of key access counts
//   S_i  : Level saturation signal (capacity + run-count thresholds)
//
// Hysteresis FSM prevents strategy thrashing via a cooldown epoch counter.
// =============================================================================
#include "common.h"
#include <chrono>
#include <deque>
#include <vector>
#include <cmath>
#include <numeric>
#include <algorithm>

namespace cascade {

enum class Strategy { LEVELING, HYBRID, TIERING };

inline const char* strategyName(Strategy s) {
    switch(s) {
        case Strategy::LEVELING: return "Leveling";
        case Strategy::HYBRID:   return "Hybrid";
        case Strategy::TIERING:  return "Tiering";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// EWMA Write Velocity tracker
// ---------------------------------------------------------------------------
struct WriteVelocityTracker {
    double alpha        = 0.3;
    double ewma_velocity = 0.0;       // bytes/sec

    using Clock = std::chrono::high_resolution_clock;
    Clock::time_point last_flush_time = Clock::now();
    std::shared_ptr<std::mutex> mu = std::make_shared<std::mutex>();

    void recordFlush(int bytes_flushed) {
        std::lock_guard<std::mutex> lk(*mu);
        auto now = Clock::now();
        double dt = std::chrono::duration<double>(now - last_flush_time).count();
        last_flush_time = now;
        if (dt <= 0) return;
        double instant = bytes_flushed / dt;
        ewma_velocity = alpha * instant + (1.0 - alpha) * ewma_velocity;
    }

    double velocity() const {
        std::lock_guard<std::mutex> lk(*mu);
        return ewma_velocity;
    }
};

// ---------------------------------------------------------------------------
// Gini Skew Estimator — sliding window of key access counts
// Gini coefficient: 0 = perfectly uniform, 1 = maximally skewed
// ---------------------------------------------------------------------------
struct GiniSkewEstimator {
    static constexpr int WINDOW = 2048;
    std::deque<Key> access_window;
    std::unordered_map<Key, int> freq;
    std::shared_ptr<std::mutex> mu = std::make_shared<std::mutex>();

    void recordAccess(Key key) {
        std::lock_guard<std::mutex> lk(*mu);
        if ((int)access_window.size() >= WINDOW) {
            Key old = access_window.front();
            access_window.pop_front();
            if (--freq[old] == 0) freq.erase(old);
        }
        access_window.push_back(key);
        freq[key]++;
    }

    double gini() const {
        std::lock_guard<std::mutex> lk(*mu);
        if (freq.empty()) return 0.0;
        std::vector<int> counts;
        counts.reserve(freq.size());
        for (auto& kv : freq) counts.push_back(kv.second);
        std::sort(counts.begin(), counts.end());
        int n = (int)counts.size();
        double sum = std::accumulate(counts.begin(), counts.end(), 0.0);
        if (sum == 0) return 0.0;
        double weighted = 0.0;
        for (int i = 0; i < n; i++)
            weighted += (2.0*(i+1) - n - 1) * counts[i];
        return weighted / (n * sum);
    }
};

// ---------------------------------------------------------------------------
// AHLC Strategy Selector with Hysteresis FSM
// ---------------------------------------------------------------------------
class AHLCEngine {
public:
    explicit AHLCEngine(const Config& cfg)
        : cfg_(cfg), current_(Strategy::HYBRID), cooldown_(0) {}

    // Called after each flush: update signals and possibly switch strategy
    Strategy evaluate(double write_velocity, double skew, bool any_level_full) {
        if (cooldown_ > 0) { cooldown_--; return current_; }

        Strategy desired;
        if (write_velocity > cfg_.ahlc_write_rate_high && any_level_full)
            desired = Strategy::TIERING;
        else if (skew > cfg_.ahlc_skew_threshold)
            desired = Strategy::LEVELING;
        else
            desired = Strategy::HYBRID;

        if (desired != current_) {
            current_ = desired;
            cooldown_ = cfg_.ahlc_hysteresis_epochs;
            // Atomic increment — safe to call from any thread without external lock
            switches_total_.fetch_add(1, std::memory_order_relaxed);
            switches_delta_.fetch_add(1, std::memory_order_relaxed);
        }
        return current_;
    }

    Strategy current()  const { return current_; }
    // Total cumulative switches since construction
    int switches()      const { return switches_total_.load(std::memory_order_relaxed); }
    // Drain pending delta — atomically returns and resets the unreported switch count.
    // Call from doCompaction() instead of the old TOCTOU compare-then-add pattern.
    int drainSwitchDelta() {
        return switches_delta_.exchange(0, std::memory_order_acq_rel);
    }
    void reset() { current_ = Strategy::HYBRID; cooldown_ = 0;
                   switches_total_.store(0); switches_delta_.store(0); }

    // Strategy-aware level parameters
    struct LevelParams { int maxRuns; int capacityMult; };
    LevelParams levelParams() const {
        switch(current_) {
            case Strategy::LEVELING: return {2, 3};
            case Strategy::HYBRID:   return {4, 4};
            case Strategy::TIERING:  return {8, 8};
        }
        return {4, 4};
    }

private:
    const Config& cfg_;
    Strategy current_;
    int      cooldown_ = 0;
    // Separate counters: total (for reporting) and delta (for doCompaction TOCTOU fix)
    std::atomic<int> switches_total_{0};
    std::atomic<int> switches_delta_{0};
};

// ---------------------------------------------------------------------------
// Compaction functions (operate on KVPair runs or real file-backed SSTables)
// ---------------------------------------------------------------------------
using Level = std::vector<std::vector<KVPair>>;  // list of sorted runs
class SSTable;
using SSTableLevel = std::vector<std::shared_ptr<SSTable>>;

// Merge all runs at `level` into one sorted run, push to level+1 (Leveling)
void compactLeveling(std::vector<Level>& levels, int level,
                     std::atomic<int64_t>& bytes_written);

void compactLeveling(std::vector<SSTableLevel>& levels, int level,
                     const std::string& db_path,
                     std::atomic<int64_t>& bytes_written,
                     int bloom_bits_per_key = 14);

// Merge runs only when run count >= maxRuns (Tiering)
void compactTiering(std::vector<Level>& levels, int level, int maxRuns,
                    std::atomic<int64_t>& bytes_written);

void compactTiering(std::vector<SSTableLevel>& levels, int level, int maxRuns,
                    const std::string& db_path,
                    std::atomic<int64_t>& bytes_written,
                    int bloom_bits_per_key = 14);

// Sub-compaction: key-range merge on [lo, hi) subset of runs (granular)
void compactSubRange(std::vector<Level>& levels, int level,
                     Key lo, Key hi,
                     std::atomic<int64_t>& bytes_written);

void compactSubRange(std::vector<SSTableLevel>& levels, int level,
                     Key lo, Key hi,
                     const std::string& db_path,
                     std::atomic<int64_t>& bytes_written,
                     int bloom_bits_per_key = 14);

// Merge k sorted runs (k-way merge with deduplication + tombstone removal)
std::vector<KVPair> mergeRuns(std::vector<std::vector<KVPair>>& runs,
                               bool remove_tombstones = false);

} // namespace cascade
