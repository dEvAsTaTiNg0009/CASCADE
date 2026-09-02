// =============================================================================
// scale_bench.cpp — 1M vs 10M operation scaling benchmark
// CASCADE (CSB+ · AHLC · Adaptive Bloom) vs Baseline (SkipList · Fixed-Leveling)
// Workload A: 50% reads / 50% updates, Zipfian theta=0.99
// =============================================================================
#include "../include/lsm.h"
#include "../include/workload.h"
#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <iomanip>

using namespace cascade;
using namespace std::chrono;

struct BenchResult {
    std::string label;
    int         scale;
    double      throughput;
    double      waf, raf, saf, bloom_fpr;
    double      p50_us, p99_us, p999_us;
    int         ahlc_switches;
    double      elapsed_sec;
};

BenchResult runWorkload(const std::string& label, int scale, Config cfg) {
    LSMEngine engine(cfg);
    YCSBWorkloadConfig wl_cfg = workloadA(scale);

    // Load phase
    auto load_ops = generateLoad(wl_cfg);
    for (auto& op : load_ops)
        engine.insert(op.key, "v" + std::to_string(op.key));
    engine.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Transaction phase
    auto ops = generateOps(wl_cfg);
    Value val;
    auto t0 = high_resolution_clock::now();
    for (auto& op : ops) {
        switch (op.type) {
            case OpType::INSERT: case OpType::UPDATE:
                engine.insert(op.key, "v" + std::to_string(op.key)); break;
            case OpType::READ:
                engine.search(op.key, val); break;
            case OpType::DELETE:
                engine.del(op.key); break;
            case OpType::SCAN:
                engine.scan(op.key, op.scan_end); break;
            case OpType::RMW:
                if (engine.search(op.key, val)) engine.insert(op.key, val + "_u"); break;
        }
    }
    auto t1 = high_resolution_clock::now();
    double elapsed = duration<double>(t1 - t0).count();

    auto& m = engine.metrics();
    BenchResult r;
    r.label = label; r.scale = scale;
    r.throughput = ops.size() / elapsed;
    r.waf = m.waf(); r.raf = m.raf(); r.saf = m.saf();
    r.bloom_fpr = m.bloom_fpr();
    r.p50_us  = m.read_latency.percentile(50)   / 1000.0;
    r.p99_us  = m.read_latency.percentile(99)   / 1000.0;
    r.p999_us = m.read_latency.percentile(99.9) / 1000.0;
    r.ahlc_switches = (int)engine.ahlc().switches();
    r.elapsed_sec = elapsed;
    return r;
}

void printTable(const std::vector<BenchResult>& res) {
    std::cout << "\n  +--------------------------+------+----------+-------+-------+-------+---------+-------+--------+-----------+\n";
    std::cout <<   "  | Config                   |Scale | Tput(K/s)| WAF   | RAF   | SAF   | BlmFPR% | P50µs | P99µs  | P99.9µs   |\n";
    std::cout <<   "  +--------------------------+------+----------+-------+-------+-------+---------+-------+--------+-----------+\n";
    for (auto& r : res) {
        std::string sc = (r.scale >= 1000000) ? std::to_string(r.scale/1000000)+"M" : std::to_string(r.scale/1000)+"K";
        std::cout << "  | " << std::left  << std::setw(24) << r.label
                  << " | " << std::right << std::setw(4)  << sc
                  << " | " << std::setw(8)  << std::fixed << std::setprecision(1) << r.throughput/1000.0
                  << " | " << std::setw(5)  << std::setprecision(2) << r.waf
                  << " | " << std::setw(5)  << r.raf
                  << " | " << std::setw(5)  << r.saf
                  << " | " << std::setw(7)  << std::setprecision(4) << r.bloom_fpr*100
                  << " | " << std::setw(5)  << std::setprecision(1) << r.p50_us
                  << " | " << std::setw(6)  << r.p99_us
                  << " | " << std::setw(9)  << r.p999_us
                  << " |\n";
    }
    std::cout <<   "  +--------------------------+------+----------+-------+-------+-------+---------+-------+--------+-----------+\n";
}

int main() {
    std::cout << "\n  +================================================================+\n";
    std::cout <<   "  |  CASCADE — 1M vs 10M Scale Comparison (Workload A: 50R/50U)  |\n";
    std::cout <<   "  |  Zipfian theta=0.99, single-threaded, Apple Silicon arm64     |\n";
    std::cout <<   "  +================================================================+\n";

    Config cascade_cfg;
    cascade_cfg.memtable_type      = MemtableType::CSB_PLUS;
    cascade_cfg.memtable_capacity  = 4096;
    cascade_cfg.max_levels         = 7;
    cascade_cfg.bloom_bits_per_key = 14;

    Config baseline_cfg;
    baseline_cfg.memtable_type        = MemtableType::SKIP_LIST;
    baseline_cfg.memtable_capacity    = 4096;
    baseline_cfg.max_levels           = 7;
    baseline_cfg.bloom_bits_per_key   = 14;
    baseline_cfg.ahlc_write_rate_high = 1e18;
    baseline_cfg.ahlc_skew_threshold  = 1.1;

    std::vector<BenchResult> results;
    for (int scale : {1000000, 10000000}) {
        std::string sc = (scale==1000000) ? "1M" : "10M";
        std::cout << "\n  [" << sc << "] CASCADE...  " << std::flush;
        results.push_back(runWorkload("CASCADE(CSB+·AHLC·Bloom)", scale, cascade_cfg));
        std::cout << "done (" << std::fixed << std::setprecision(1) << results.back().elapsed_sec << "s)\n";

        std::cout <<   "  [" << sc << "] Baseline... " << std::flush;
        results.push_back(runWorkload("Baseline(Skip·Lvl·Unif)", scale, baseline_cfg));
        std::cout << "done (" << std::fixed << std::setprecision(1) << results.back().elapsed_sec << "s)\n";
    }

    std::cout << "\n  === Full Results Table ===";
    printTable(results);

    // Gain summary
    std::cout << "\n  === CASCADE vs Baseline: Improvement Summary ===\n";
    std::cout << "  +-------+------------+----------+----------+----------+\n";
    std::cout << "  | Scale | Tput Gain% | ΔWAF     | ΔRAF     | ΔSAF     |\n";
    std::cout << "  +-------+------------+----------+----------+----------+\n";
    for (int i = 0; i < (int)results.size(); i += 2) {
        auto& c = results[i]; auto& b = results[i+1];
        std::string sc = (c.scale==1000000)?"1M":"10M";
        double tg = b.throughput>0?(c.throughput-b.throughput)/b.throughput*100:0;
        double wg = b.waf>0?(c.waf-b.waf)/b.waf*100:0;
        double rg = b.raf>0?(c.raf-b.raf)/b.raf*100:0;
        double sg = b.saf>0?(c.saf-b.saf)/b.saf*100:0;
        std::cout << "  | " << std::left  << std::setw(5) << sc
                  << " | +" << std::right << std::setw(9) << std::fixed << std::setprecision(1) << tg << "%"
                  << " | " << std::setw(7) << wg << "% | "
                  << std::setw(7) << rg << "% | "
                  << std::setw(7) << sg << "% |\n";
    }
    std::cout << "  +-------+------------+----------+----------+----------+\n";
    std::cout << "  Note: Negative ΔWAF/ΔRAF/ΔSAF = CASCADE has lower amplification (better)\n";
    return 0;
}
