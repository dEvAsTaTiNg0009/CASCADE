#pragma once
// =============================================================================
// workload.h — YCSB Workload Generators + Scrambled Zipfian Distribution
//
// Standard YCSB workloads: A, B, C, D, E, F
// Paper-defined workload extensions (not part of standard YCSB spec):
//   W   (1R/99W)       — write-dominated ingest (APM/telemetry)
//   RW  (50R/50W)      — balanced read/write (social/collaboration)
//   RSW (25R/25W/50S)  — scan-heavy reporting; scan_length matches Workload E
//   RS  (47R/47W/6S)   — mixed scans analytics; scan_length matches Workload E
//   R   (95R/5W)       — read-heavy caching
//
// Seed discipline: generateOps(cfg, seed) accepts an explicit seed.
// The benchmark harness passes seed = BASE_SEED + run_id (same seed per
// run number across all 11 workloads) so cross-workload comparisons stay valid.
// =============================================================================
#include "common.h"
#include <vector>
#include <string>
#include <random>
#include <cmath>
#include <stdexcept>

namespace cascade {

// ---------------------------------------------------------------------------
// Scrambled Zipfian generator (theta = skew parameter, 0 = uniform, 1 = extreme)
// Based on YCSB's ScrambledZipfianGenerator
// ---------------------------------------------------------------------------
class ZipfianGenerator {
public:
    ZipfianGenerator(uint64_t items, double theta, uint64_t seed = 42)
        : items_(items), theta_(theta), rng_(seed)
    {
        if (theta == 0.0) { uniform_ = true; return; }
        zeta_n_ = zeta(items, theta);
        zeta_2_ = zeta(2, theta);
        alpha_  = 1.0 / (1.0 - theta);
        eta_    = (1.0 - std::pow(2.0 / items_, 1.0 - theta)) /
                  (1.0 - zeta_2_ / zeta_n_);
    }

    uint64_t next() {
        if (uniform_) {
            std::uniform_int_distribution<uint64_t> dist(0, items_ - 1);
            return scramble(dist(rng_));
        }
        std::uniform_real_distribution<double> udist(0.0, 1.0);
        double u = udist(rng_);
        double uz = u * zeta_n_;
        uint64_t ret;
        if (uz < 1.0)        ret = 0;
        else if (uz < 1.0 + std::pow(0.5, theta_)) ret = 1;
        else ret = (uint64_t)(items_ * std::pow(eta_ * u - eta_ + 1.0, alpha_));
        if (ret >= items_) ret = items_ - 1;
        return scramble(ret);
    }

private:
    uint64_t items_;
    double   theta_;
    double   zeta_n_, zeta_2_, alpha_, eta_;
    bool     uniform_ = false;
    std::mt19937_64 rng_;

    uint64_t scramble(uint64_t z) { return hash64(z) % items_; }

    static double zeta(uint64_t n, double theta) {
        double s = 0.0;
        for (uint64_t i = 1; i <= n; i++)
            s += 1.0 / std::pow((double)i, theta);
        return s;
    }
};

// ---------------------------------------------------------------------------
// YCSB Workload definitions
// ---------------------------------------------------------------------------
enum class OpType { INSERT, READ, UPDATE, DELETE, SCAN, RMW };

struct Op {
    OpType   type;
    Key      key;
    Key      scan_end = 0; // for SCAN
};

struct YCSBWorkloadConfig {
    std::string name;
    int         num_ops       = 100000;
    int         record_count  = 50000;
    uint64_t    key_space     = 1000000;
    double      zipfian_theta = 0.99;
    // Operation mix (must sum to 1.0)
    double read_frac   = 0.5;
    double insert_frac = 0.0;
    double update_frac = 0.5;
    double delete_frac = 0.0;
    double scan_frac   = 0.0;
    double rmw_frac    = 0.0;
    int    scan_length = 100; // keys per scan
};

// Predefined YCSB workloads A–F
inline YCSBWorkloadConfig workloadA(int ops = 100000) {
    uint64_t ks = std::max((uint64_t)1000000, (uint64_t)ops * 2);
    return {"A(50R/50U)", ops, ops/2, ks, 0.99, 0.5, 0.0, 0.5};
}
inline YCSBWorkloadConfig workloadB(int ops = 100000) {
    uint64_t ks = std::max((uint64_t)1000000, (uint64_t)ops * 2);
    return {"B(95R/5U)", ops, ops/2, ks, 0.99, 0.95, 0.0, 0.05};
}
inline YCSBWorkloadConfig workloadC(int ops = 100000) {
    uint64_t ks = std::max((uint64_t)1000000, (uint64_t)ops * 2);
    return {"C(100R)", ops, ops/2, ks, 0.99, 1.0, 0.0, 0.0};
}
inline YCSBWorkloadConfig workloadD(int ops = 100000) {
    uint64_t ks = std::max((uint64_t)1000000, (uint64_t)ops * 2);
    return {"D(95R/5I)", ops, ops/2, ks, 0.0, 0.95, 0.05, 0.0};
}
inline YCSBWorkloadConfig workloadE(int ops = 100000) {
    uint64_t ks = std::max((uint64_t)1000000, (uint64_t)ops * 2);
    YCSBWorkloadConfig c;
    c.name = "E(95Scan/5I)"; c.num_ops = ops; c.record_count = ops/2; c.key_space = ks;
    c.read_frac = 0.0; c.scan_frac = 0.95; c.insert_frac = 0.05;
    return c;
}
inline YCSBWorkloadConfig workloadF(int ops = 100000) {
    uint64_t ks = std::max((uint64_t)1000000, (uint64_t)ops * 2);
    YCSBWorkloadConfig c;
    c.name = "F(50R/50RMW)"; c.num_ops = ops; c.record_count = ops/2; c.key_space = ks;
    c.read_frac = 0.5; c.rmw_frac = 0.5;
    return c;
}

// ---------------------------------------------------------------------------
// Paper-defined workload extensions — NOT part of the standard YCSB spec.
// Defined explicitly here and in the paper's methodology section.
// All use zipfian_theta=0.99 and same key_space/record_count conventions as A–F.
// RSW and RS set scan_length=100 to match Workload E's scan settings.
// ---------------------------------------------------------------------------

// W: Write-dominated ingest — models APM/telemetry pipelines.
// 1% reads, 99% updates. Stresses the write path harder than A or F.
inline YCSBWorkloadConfig workloadW(int ops = 100000) {
    uint64_t ks = std::max((uint64_t)1000000, (uint64_t)ops * 2);
    YCSBWorkloadConfig c;
    c.name = "W(1R/99W)"; c.num_ops = ops; c.record_count = ops/2; c.key_space = ks;
    c.zipfian_theta = 0.99;
    c.read_frac = 0.01; c.update_frac = 0.99;
    return c;
}

// RW: Balanced read/write — models social/collaboration systems.
// 50% reads, 50% updates (writes only, no RMW).
inline YCSBWorkloadConfig workloadRW(int ops = 100000) {
    uint64_t ks = std::max((uint64_t)1000000, (uint64_t)ops * 2);
    YCSBWorkloadConfig c;
    c.name = "RW(50R/50W)"; c.num_ops = ops; c.record_count = ops/2; c.key_space = ks;
    c.zipfian_theta = 0.99;
    c.read_frac = 0.50; c.update_frac = 0.50;
    return c;
}

// RSW: Scan-heavy reporting — 25% reads, 25% writes, 50% scans.
// scan_length=100 matches Workload E for consistency.
inline YCSBWorkloadConfig workloadRSW(int ops = 100000) {
    uint64_t ks = std::max((uint64_t)1000000, (uint64_t)ops * 2);
    YCSBWorkloadConfig c;
    c.name = "RSW(25R/25W/50S)"; c.num_ops = ops; c.record_count = ops/2; c.key_space = ks;
    c.zipfian_theta = 0.99;
    c.read_frac = 0.25; c.update_frac = 0.25; c.scan_frac = 0.50;
    c.scan_length = 100; // matches Workload E
    return c;
}

// RS: Mixed scans analytics — 47% reads, 47% writes, 6% scans.
// scan_length=100 matches Workload E for consistency.
inline YCSBWorkloadConfig workloadRS(int ops = 100000) {
    uint64_t ks = std::max((uint64_t)1000000, (uint64_t)ops * 2);
    YCSBWorkloadConfig c;
    c.name = "RS(47R/47W/6S)"; c.num_ops = ops; c.record_count = ops/2; c.key_space = ks;
    c.zipfian_theta = 0.99;
    c.read_frac = 0.47; c.update_frac = 0.47; c.scan_frac = 0.06;
    c.scan_length = 100; // matches Workload E
    return c;
}

// R: Read-heavy caching — 95% reads, 5% writes.
// Models a caching tier where writes are rare updates.
inline YCSBWorkloadConfig workloadR(int ops = 100000) {
    uint64_t ks = std::max((uint64_t)1000000, (uint64_t)ops * 2);
    YCSBWorkloadConfig c;
    c.name = "R(95R/5W)"; c.num_ops = ops; c.record_count = ops/2; c.key_space = ks;
    c.zipfian_theta = 0.99;
    c.read_frac = 0.95; c.update_frac = 0.05;
    return c;
}

// Generate operation sequence for a workload
inline std::vector<Op> generateOps(const YCSBWorkloadConfig& cfg, uint64_t seed = 42) {
    std::mt19937_64 rng(seed);
    ZipfianGenerator zipf(cfg.key_space, cfg.zipfian_theta, seed + 1);
    std::uniform_real_distribution<double> udist(0.0, 1.0);
    std::uniform_int_distribution<int> scan_len(1, cfg.scan_length);

    std::vector<Op> ops;
    ops.reserve(cfg.num_ops);

    for (int i = 0; i < cfg.num_ops; i++) {
        double r = udist(rng);
        OpType t;
        if      (r < cfg.read_frac)                               t = OpType::READ;
        else if (r < cfg.read_frac + cfg.update_frac)             t = OpType::UPDATE;
        else if (r < cfg.read_frac + cfg.update_frac + cfg.insert_frac) t = OpType::INSERT;
        else if (r < cfg.read_frac + cfg.update_frac + cfg.insert_frac + cfg.scan_frac) t = OpType::SCAN;
        else if (r < cfg.read_frac + cfg.update_frac + cfg.insert_frac + cfg.scan_frac + cfg.delete_frac) t = OpType::DELETE;
        else                                                       t = OpType::RMW;

        Key k = zipf.next() + 1; // avoid key=0
        Op op{t, k};
        if (t == OpType::SCAN) op.scan_end = k + scan_len(rng);
        ops.push_back(op);
    }
    return ops;
}

// Generate initial load (bulk inserts)
inline std::vector<Op> generateLoad(const YCSBWorkloadConfig& cfg, uint64_t seed = 99) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<Key> dist(1, cfg.key_space);
    std::vector<Op> ops;
    ops.reserve(cfg.record_count);
    for (int i = 0; i < cfg.record_count; i++)
        ops.push_back({OpType::INSERT, dist(rng)});
    return ops;
}

} // namespace cascade
