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
        // Sort-based Gini: O(n log n), NOT the naive O(n²) double-sum.
        // Equivalent to the paper's double-sum definition but runs in
        // O(n log n) via the standard sorted-rank formulation:
        //   G = (2 * sum_i (i+1)*x_i - (n+1)*sum_i x_i) / (n * sum_i x_i)
        // where x_i are sorted counts (ascending, 0-indexed).
        std::sort(counts.begin(), counts.end());
        int n = (int)counts.size();
        double sum = std::accumulate(counts.begin(), counts.end(), 0.0);
        if (sum == 0) return 0.0;
#ifdef CASCADE_AHLC_TIMING
        auto _t0 = std::chrono::high_resolution_clock::now();
#endif
        double weighted = 0.0;
        for (int i = 0; i < n; i++)
            weighted += (2.0*(i+1) - n - 1) * counts[i];
        double result = weighted / (n * sum);
#ifdef CASCADE_AHLC_TIMING
        auto _t1 = std::chrono::high_resolution_clock::now();
        double _ms = std::chrono::duration<double,std::milli>(_t1-_t0).count();
        // Print only when timing is enabled — not in normal benchmark mode
        fprintf(stderr, "[AHLC_TIMING] gini n=%d sum=%.0f result=%.4f time=%.3fms\n",
                n, sum, result, _ms);
#endif
        return result;
    }
};

// ---------------------------------------------------------------------------
// Full diagnostic record for a single AHLC evaluation point.
// Captured on every strategy SWITCH; available via AHLCEngine::lastDiagnostic()
// for tests and benchmarks. Does NOT affect the decision logic itself.
// ---------------------------------------------------------------------------
struct AHLCDiagnostic {
    double   timestamp_ms        = 0.0;
    Strategy prev_strategy       = Strategy::HYBRID;
    Strategy new_strategy        = Strategy::HYBRID;
    // Velocity signal
    double   ewma_write_velocity = 0.0;  // bytes/sec (EWMA)
    double   velocity_threshold  = 0.0;  // cfg.ahlc_write_rate_high (bytes/sec)
    bool     velocity_condition  = false; // (ewma_write_velocity > velocity_threshold)
    // Saturation signal
    bool     any_level_full      = false;
    int      first_full_level    = -1;   // index of first full level, or -1
    // Skew signal
    double   skew                = 0.0;
    double   skew_threshold      = 0.0;  // cfg.ahlc_skew_threshold
    bool     skew_condition      = false; // (skew > skew_threshold)
    // Hysteresis / FSM state
    int      cooldown_at_decision = 0;   // cooldown counter value when decision was made
    bool     transition_occurred  = false;
    // Effective decision path
    // "TIERING": velocity_condition && any_level_full
    // "LEVELING": skew_condition
    // "HYBRID":   default
    std::string decision_reason;         // human-readable
};

// StrategySwitchLog: backward-compatible switch-only log entry
struct StrategySwitchLog {
    double   timestamp_ms;
    Strategy from_strategy;
    Strategy to_strategy;
    double   write_velocity;
    double   skew;
    // Extended fields (populated alongside AHLCDiagnostic)
    double   velocity_threshold  = 0.0;
    bool     velocity_condition  = false;
    bool     any_level_full      = false;
    double   skew_threshold      = 0.0;
    bool     skew_condition      = false;
    int      cooldown_at_decision = 0;
};

// ---------------------------------------------------------------------------
// AHLC Strategy Selector with Hysteresis FSM
// ---------------------------------------------------------------------------
class AHLCEngine {
public:
    explicit AHLCEngine(const Config& cfg)
        : cfg_(cfg), current_(Strategy::HYBRID), cooldown_(0),
          start_time_(std::chrono::high_resolution_clock::now()) {}

    // Called after each flush: update signals and possibly switch strategy.
    // Decision logic is UNCHANGED.  All signals are captured in last_diag_
    // so tests and benchmarks can inspect the full decision context.
    Strategy evaluate(double write_velocity, double skew, bool any_level_full,
                      int first_full_level = -1) {
        bool vel_cond  = (write_velocity > cfg_.ahlc_write_rate_high);
        bool skew_cond = (skew > cfg_.ahlc_skew_threshold);

        // Build diagnostic record BEFORE applying cooldown so we capture the
        // signals even when the FSM is in the cooldown state.
        AHLCDiagnostic diag;
        {
            auto now = std::chrono::high_resolution_clock::now();
            diag.timestamp_ms        = std::chrono::duration<double,std::milli>(now - start_time_).count();
            diag.prev_strategy       = current_;
            diag.ewma_write_velocity = write_velocity;
            diag.velocity_threshold  = cfg_.ahlc_write_rate_high;
            diag.velocity_condition  = vel_cond;
            diag.any_level_full      = any_level_full;
            diag.first_full_level    = first_full_level;
            diag.skew                = skew;
            diag.skew_threshold      = cfg_.ahlc_skew_threshold;
            diag.skew_condition      = skew_cond;
            diag.cooldown_at_decision = cooldown_;
        }

        if (cooldown_ > 0) {
            cooldown_--;
            diag.new_strategy       = current_;
            diag.transition_occurred = false;
            diag.decision_reason    = std::string("cooldown=") + std::to_string(cooldown_ + 1);
            std::lock_guard<std::mutex> lk(log_mu_);
            last_diag_ = diag;
            return current_;
        }

        Strategy desired;
        if (vel_cond && any_level_full) {
            desired = Strategy::TIERING;
            diag.decision_reason = "velocity+saturation";
        } else if (skew_cond) {
            desired = Strategy::LEVELING;
            diag.decision_reason = "high_skew";
        } else {
            desired = Strategy::HYBRID;
            diag.decision_reason = "default";
        }
        diag.new_strategy = desired;

        if (desired != current_) {
            diag.transition_occurred = true;
            {
                std::lock_guard<std::mutex> lk(log_mu_);
                double ms = diag.timestamp_ms;
                StrategySwitchLog entry;
                entry.timestamp_ms        = ms;
                entry.from_strategy       = current_;
                entry.to_strategy         = desired;
                entry.write_velocity      = write_velocity;
                entry.skew                = skew;
                entry.velocity_threshold  = cfg_.ahlc_write_rate_high;
                entry.velocity_condition  = vel_cond;
                entry.any_level_full      = any_level_full;
                entry.skew_threshold      = cfg_.ahlc_skew_threshold;
                entry.skew_condition      = skew_cond;
                entry.cooldown_at_decision = 0;
                switch_log_.push_back(entry);
                last_diag_ = diag;
            }
            current_ = desired;
            cooldown_ = cfg_.ahlc_hysteresis_epochs;
            // Atomic increment — safe to call from any thread without external lock
            switches_total_.fetch_add(1, std::memory_order_relaxed);
            switches_delta_.fetch_add(1, std::memory_order_relaxed);
        } else {
            std::lock_guard<std::mutex> lk(log_mu_);
            last_diag_ = diag;
        }
        return current_;
    }

    Strategy current()  const { return current_; }
    // Total cumulative switches since construction
    int switches()      const { return switches_total_.load(std::memory_order_relaxed); }
    // Full diagnostic record from the most recent evaluate() call
    AHLCDiagnostic lastDiagnostic() const {
        std::lock_guard<std::mutex> lk(log_mu_);
        return last_diag_;
    }
    std::vector<StrategySwitchLog> switchLog() const {
        std::lock_guard<std::mutex> lk(log_mu_);
        return switch_log_;
    }
    // Drain pending delta — atomically returns and resets the unreported switch count.
    // Call from doCompaction() instead of the old TOCTOU compare-then-add pattern.
    int drainSwitchDelta() {
        return switches_delta_.exchange(0, std::memory_order_acq_rel);
    }
    void reset() {
        current_ = Strategy::HYBRID; cooldown_ = 0;
        switches_total_.store(0); switches_delta_.store(0);
        std::lock_guard<std::mutex> lk(log_mu_);
        switch_log_.clear();
        last_diag_ = AHLCDiagnostic{};
        start_time_ = std::chrono::high_resolution_clock::now();
    }

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
    mutable std::mutex log_mu_;
    std::vector<StrategySwitchLog> switch_log_;
    AHLCDiagnostic   last_diag_;   // most recent evaluate() diagnostic snapshot
    std::chrono::high_resolution_clock::time_point start_time_;
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
