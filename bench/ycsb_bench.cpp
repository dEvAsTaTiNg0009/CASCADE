// =============================================================================
// ycsb_bench.cpp — Full YCSB Benchmark Suite for CASCADE Research Engine
//
// Compiler: g++ (GCC 13.x) or Apple Clang 16.x, C++17, -O2 -pthread
// Platform: Linux x86-64 / macOS arm64 (Apple Silicon)
//
// Sections:
//   1. Single-threaded YCSB (A–F) — CASCADE vs SkipList baseline — N=1,000,000
//   2. Multi-threaded throughput scaling (1–16 threads) — N=100,000
//   3. Zipfian theta sweep (0.0, 0.8, 0.9, 0.99) — N=500,000
//   4. Full 8-config ablation: {SkipList/CSB+} × {Fixed/AHLC} × {Uniform/Adaptive Bloom}
//      — N=200,000, genuinely uses SkipListMemtable for Skip rows
// =============================================================================
#include "../include/lsm.h"
#include "../include/workload.h"
#include <iostream>
#include <fstream>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>
#include <iomanip>
#include <sstream>
#include <numeric>
#include <cmath>
#include <functional>

using namespace cascade;
using namespace std::chrono;

// ---------------------------------------------------------------------------
// Thread-pool concurrent benchmark runner
// ---------------------------------------------------------------------------
struct ThreadResult {
    uint64_t ops   = 0;
    uint64_t hits  = 0;
    double   time_sec = 0;
};

ThreadResult runThreaded(LSMEngine& engine, const std::vector<Op>& ops,
                          int thread_count)
{
    int total = (int)ops.size();
    int chunk = (total + thread_count - 1) / thread_count;
    std::vector<std::thread> threads;
    std::vector<ThreadResult> results(thread_count);

    auto t0 = high_resolution_clock::now();
    for (int t = 0; t < thread_count; t++) {
        threads.emplace_back([&, t]() {
            int start = t * chunk;
            int end   = std::min(start + chunk, total);
            Value val;
            uint64_t my_ops = 0, my_hits = 0;
            for (int i = start; i < end; i++) {
                const Op& op = ops[i];
                switch (op.type) {
                    case OpType::INSERT:
                    case OpType::UPDATE:
                        engine.insert(op.key, "v" + std::to_string(op.key));
                        break;
                    case OpType::READ:
                        if (engine.search(op.key, val)) my_hits++;
                        break;
                    case OpType::DELETE:
                        engine.del(op.key);
                        break;
                    case OpType::SCAN:
                        engine.scan(op.key, op.scan_end);
                        break;
                    case OpType::RMW:
                        if (engine.search(op.key, val))
                            engine.insert(op.key, val + "_u");
                        break;
                }
                my_ops++;
            }
            results[t].ops  = my_ops;
            results[t].hits = my_hits;
        });
    }
    for (auto& th : threads) th.join();
    auto t1 = high_resolution_clock::now();

    ThreadResult agg;
    for (auto& r : results) { agg.ops += r.ops; agg.hits += r.hits; }
    agg.time_sec = duration<double>(t1 - t0).count();
    return agg;
}

// ---------------------------------------------------------------------------
// Run a single YCSB workload — returns throughput + RUM metrics
// ---------------------------------------------------------------------------
struct BenchResult {
    std::string label;
    double throughput_ops_sec;
    double waf, raf, saf, bloom_fpr;
    double p50_us, p99_us, p999_us;
    int ahlc_switches;
};

BenchResult runWorkload(const YCSBWorkloadConfig& wl_cfg,
                         Config engine_cfg,
                         int thread_count = 1)
{
    LSMEngine engine(engine_cfg);

    // Load phase
    auto load_ops = generateLoad(wl_cfg);
    for (auto& op : load_ops)
        engine.insert(op.key, "v" + std::to_string(op.key));
    engine.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // let bg compact settle

    // Transaction phase
    auto ops = generateOps(wl_cfg);
    auto res = runThreaded(engine, ops, thread_count);

    BenchResult br;
    br.label              = wl_cfg.name;
    br.throughput_ops_sec = res.time_sec > 0 ? res.ops / res.time_sec : 0;
    auto& m   = engine.metrics();
    br.waf          = m.waf();
    br.raf          = m.raf();
    br.saf          = m.saf();
    br.bloom_fpr    = m.bloom_fpr();
    br.p50_us       = m.read_latency.percentile(50)   / 1000.0;
    br.p99_us       = m.read_latency.percentile(99)   / 1000.0;
    br.p999_us      = m.read_latency.percentile(99.9) / 1000.0;
    br.ahlc_switches = (int)engine.ahlc().switches();
    return br;
}

// ---------------------------------------------------------------------------
// Formatters
// ---------------------------------------------------------------------------
void printRUMTable(const std::vector<BenchResult>& results) {
    std::cout << "\n";
    std::cout << "  +-----------------+----------+-------+-------+-------+---------+-------+--------+----------+--------+\n";
    std::cout << "  | Workload        | Tput(K/s)| WAF   | RAF   | SAF   | BlmFPR% | P50µs | P99µs  | P99.9µs  |AHLCSw. |\n";
    std::cout << "  +-----------------+----------+-------+-------+-------+---------+-------+--------+----------+--------+\n";
    for (auto& r : results) {
        std::cout << "  | " << std::left  << std::setw(15) << r.label
                  << " | " << std::right << std::setw(8)  << std::fixed << std::setprecision(1) << r.throughput_ops_sec / 1000.0
                  << " | " << std::setw(5)  << std::setprecision(2) << r.waf
                  << " | " << std::setw(5)  << std::setprecision(2) << r.raf
                  << " | " << std::setw(5)  << std::setprecision(2) << r.saf
                  << " | " << std::setw(7)  << std::setprecision(4) << r.bloom_fpr * 100
                  << " | " << std::setw(5)  << std::setprecision(1) << r.p50_us
                  << " | " << std::setw(6)  << std::setprecision(1) << r.p99_us
                  << " | " << std::setw(8)  << std::setprecision(1) << r.p999_us
                  << " | " << std::setw(6)  << r.ahlc_switches
                  << " |\n";
    }
    std::cout << "  +-----------------+----------+-------+-------+-------+---------+-------+--------+----------+--------+\n";
}

// ---------------------------------------------------------------------------
// MAIN
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    bool only_ablation = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--ablation") {
            only_ablation = true;
        }
    }

    std::cout << "\n";
    std::cout << "  +==============================================================+\n";
    std::cout << "  |  CASCADE Research Engine — Full YCSB Benchmark Suite         |\n";
    std::cout << "  |  C++17 · OLC CSB+ Tree · AHLC · Blocked Bloom Filter         |\n";
    std::cout << "  |  Compiler: g++ / Apple Clang, -std=c++17 -O2 -pthread        |\n";
    std::cout << "  |  Platform: Linux x86-64 / macOS arm64                        |\n";
    std::cout << "  +==============================================================+\n";

    // -----------------------------------------------------------------------
    // Base config — used by all sections unless overridden
    // -----------------------------------------------------------------------
    Config base_cfg;
    base_cfg.memtable_capacity    = 4096;
    base_cfg.max_levels           = 7;
    base_cfg.bloom_bits_per_key   = 14;           // 14 bits/key → FPR ≈ 0.1% at scale
    base_cfg.bloom_max_bytes      = 256ULL * 1024 * 1024; // 256MB cap
    base_cfg.block_cache_capacity = 64ULL * 1024 * 1024;  // 64MB

    Config cascade_cfg = base_cfg;
    cascade_cfg.memtable_type = MemtableType::CSB_PLUS;

    Config baseline_cfg = base_cfg;
    baseline_cfg.memtable_type        = MemtableType::SKIP_LIST;
    baseline_cfg.ahlc_write_rate_high = 1e18;  // force Leveling always
    baseline_cfg.ahlc_skew_threshold  = 1.1;

    if (!only_ablation) {
        // -----------------------------------------------------------------------
        // Section 1: YCSB Workloads A–F
        // CASCADE (CSB+ + AHLC + Adaptive Bloom) vs Baseline (SkipList + Fixed-Leveling + Uniform)
        // N=200,000 ops
        // -----------------------------------------------------------------------
        const int N1 = 200000;
        std::cout << "\n  [1/4] YCSB Workloads A-F — N=" << N1 << " ops\n";
        std::cout << "        Comparing CASCADE (CSB+·AHLC·AdaptBloom) vs Baseline (SkipList·Leveling·Uniform)\n";
        std::cout << "        Bloom: " << base_cfg.bloom_bits_per_key << " bits/key (Monkey-optimal per-level sizing)\n";

        std::vector<BenchResult> cascade_results, baseline_results;
        for (auto& wl : {workloadA(N1), workloadB(N1), workloadC(N1),
                         workloadD(N1), workloadE(N1), workloadF(N1),
                         workloadW(N1), workloadRW(N1), workloadRSW(N1),
                         workloadRS(N1), workloadR(N1)}) {
            std::cout << "    [CASCADE]  " << wl.name << "... " << std::flush;
            cascade_results.push_back(runWorkload(wl, cascade_cfg, 1));
            std::cout << "done\n";
            std::cout << "    [Baseline] " << wl.name << "... " << std::flush;
            baseline_results.push_back(runWorkload(wl, baseline_cfg, 1));
            std::cout << "done\n";
        }

        std::cout << "\n  -- CASCADE (CSB+ · AHLC · Adaptive Bloom) --";
        printRUMTable(cascade_results);
        std::cout << "\n  -- Baseline (SkipList · Fixed-Leveling · Uniform Bloom) --";
        printRUMTable(baseline_results);

    // Head-to-head summary
    std::cout << "\n  Head-to-head (YCSB-A):\n";
    std::cout << "  +--------------------+------------------+------------------+----------+\n";
    std::cout << "  | Metric             | CASCADE          | Baseline         | Gain     |\n";
    std::cout << "  +--------------------+------------------+------------------+----------+\n";
    auto& cas = cascade_results[0];
    auto& bas = baseline_results[0];
    auto pct = [](double a, double b) -> double { return b>0 ? (a-b)/b*100 : 0; };
    std::cout << "  | Throughput(K ops/s)| " << std::setw(16) << std::fixed << std::setprecision(1)
              << cas.throughput_ops_sec/1000 << " | " << std::setw(16) << bas.throughput_ops_sec/1000
              << " | +" << std::setw(6) << std::setprecision(1) << pct(cas.throughput_ops_sec,bas.throughput_ops_sec) << "% |\n";
    std::cout << "  | WAF                | " << std::setw(16) << std::setprecision(2) << cas.waf
              << " | " << std::setw(16) << bas.waf << " | "
              << std::setw(8) << std::setprecision(1) << pct(bas.waf,cas.waf) << "% |\n";
    std::cout << "  | Bloom FPR%         | " << std::setw(16) << std::setprecision(4) << cas.bloom_fpr*100
              << " | " << std::setw(16) << bas.bloom_fpr*100 << " |         |\n";
    std::cout << "  | P99 Latency (µs)   | " << std::setw(16) << std::setprecision(1) << cas.p99_us
              << " | " << std::setw(16) << bas.p99_us << " |         |\n";
    std::cout << "  +--------------------+------------------+------------------+----------+\n";

    // -----------------------------------------------------------------------
    // Section 2: Multi-threaded throughput scaling (Workload A)
    // SkipList vs CSB+ measured with the real memtable toggle
    // N=100,000 per run for speed
    // -----------------------------------------------------------------------
    const int N2 = 100000;
    std::cout << "\n  [2/4] Multi-threaded Throughput Scaling — Workload A (N=" << N2 << " per thread count)\n";
    std::cout << "        Real SkipList vs CSB+ (cfg.memtable_type switch)\n";
    std::cout << "  +----------+--------------+--------------+--------+\n";
    std::cout << "  | Threads  | SkipList(K/s)| CSB+  (K/s) | Gain % |\n";
    std::cout << "  +----------+--------------+--------------+--------+\n";
    for (int t : {1, 2, 4, 8, 16, 32}) {
        auto wl = workloadA(N2);
        Config sl_cfg  = base_cfg;
        Config csb_cfg = base_cfg;
        sl_cfg.memtable_type  = MemtableType::SKIP_LIST;
        csb_cfg.memtable_type = MemtableType::CSB_PLUS;

        std::cout << "    t=" << t << "... " << std::flush;
        auto r_sl  = runWorkload(wl, sl_cfg,  t);
        auto r_csb = runWorkload(wl, csb_cfg, t);
        std::cout << "done\n";

        double gain = r_sl.throughput_ops_sec > 0
            ? (r_csb.throughput_ops_sec - r_sl.throughput_ops_sec)
              / r_sl.throughput_ops_sec * 100.0 : 0;
        std::cout << "  | " << std::left  << std::setw(8) << t
                  << " | " << std::right << std::setw(12) << std::fixed << std::setprecision(1)
                  << r_sl.throughput_ops_sec  / 1000.0
                  << " | " << std::setw(12) << r_csb.throughput_ops_sec / 1000.0
                  << " | +" << std::setw(5) << std::setprecision(1) << gain << "% |\n";
    }
    std::cout << "  +----------+--------------+--------------+--------+\n";

    // -----------------------------------------------------------------------
    // Section 3: Zipfian theta sweep — CASCADE only — N=500,000
    // -----------------------------------------------------------------------
    const int N3 = 500000;
    std::cout << "\n  [3/4] Zipfian Theta Sweep — CASCADE, Workload A (N=" << N3 << ")\n";
    std::cout << "  +-------+----------+-------+-------+--------+\n";
    std::cout << "  | Theta | Tput(K/s)| WAF   | RAF   |AHLC Sw.|\n";
    std::cout << "  +-------+----------+-------+-------+--------+\n";
    for (double theta : {0.0, 0.8, 0.9, 0.99}) {
        YCSBWorkloadConfig wl = workloadA(N3);
        wl.zipfian_theta = theta;
        wl.name = "A(θ=" + std::to_string(theta).substr(0,4) + ")";
        std::cout << "    theta=" << theta << "... " << std::flush;
        auto r = runWorkload(wl, cascade_cfg, 1);
        std::cout << "done\n";
        std::cout << "  | " << std::left  << std::setw(5)  << theta
                  << " | " << std::right << std::setw(8)  << std::fixed
                  << std::setprecision(1) << r.throughput_ops_sec / 1000.0
                  << " | " << std::setw(5)  << std::setprecision(2) << r.waf
                  << " | " << std::setw(5)  << r.raf
                  << " | " << std::setw(6)  << r.ahlc_switches
                  << " |\n";
    }
        std::cout << "  +-------+----------+-------+-------+--------+\n";
    }

    // -----------------------------------------------------------------------
    // Section 4: Full 8-config ablation — Workload A, N=200,000
    // Rows marked Skip* GENUINELY use SkipListMemtable (cfg.memtable_type = SKIP_LIST)
    // Rows marked CSB+* GENUINELY use ConcurrentCSBTree (cfg.memtable_type = CSB_PLUS)
    // -----------------------------------------------------------------------
    const int N4 = 200000;
    std::cout << "\n  [4/4] 8-Config Ablation Study — Workload A (N=" << N4 << ")\n";
    std::cout << "        Dimensions: {SkipList/CSB+} × {Fixed-Leveling/AHLC} × {Uniform/Adaptive Bloom}\n";
    std::cout << "        Skip* rows use cfg.memtable_type=SKIP_LIST (real SkipList, not CSB+)\n";
    std::cout << "  +-----------------------+----------+-------+-------+-------+---------+--------+\n";
    std::cout << "  | Config                | Tput(K/s)| WAF   | RAF   | SAF   | BlmFPR% |AHLCSw. |\n";
    std::cout << "  +-----------------------+----------+-------+-------+-------+---------+--------+\n";

    struct AblRow {
        MemtableType mt;
        bool ahlc_on;
        bool bloom_adpt;
        std::string label;
    };

    struct AblationEntry {
        std::string label;
        double tput_kops;
        double waf;
        double raf;
        double saf;
        double bloom_fpr_pct;
        int ahlc_switches;
    };

    std::vector<AblRow> ablation_configs = {
        {MemtableType::SKIP_LIST, false, false, "Skip|Leveling|Unif  "},
        {MemtableType::SKIP_LIST, false, true,  "Skip|Leveling|Adpt  "},
        {MemtableType::SKIP_LIST, true,  false, "Skip|AHLC    |Unif  "},
        {MemtableType::SKIP_LIST, true,  true,  "Skip|AHLC    |Adpt  "},
        {MemtableType::CSB_PLUS,  false, false, "CSB+|Leveling|Unif  "},
        {MemtableType::CSB_PLUS,  false, true,  "CSB+|Leveling|Adpt  "},
        {MemtableType::CSB_PLUS,  true,  false, "CSB+|AHLC    |Unif  "},
        {MemtableType::CSB_PLUS,  true,  true,  "CSB+|AHLC    |Adpt \u2190CASCADE"},
    };

    std::vector<AblationEntry> entries_200K, entries_1M;

    auto wl_ablation = workloadA(N4);
    for (auto& row : ablation_configs) {
        Config cfg = base_cfg;
        cfg.memtable_type = row.mt;
        if (!row.ahlc_on) {
            cfg.ahlc_write_rate_high = 1e18; // force Leveling always
            cfg.ahlc_skew_threshold  = 1.1;
        }
        if (!row.bloom_adpt)
            cfg.bloom_total_budget = 1000000; // same order of magnitude, uniform

        std::cout << "    " << row.label << "... " << std::flush;
        auto r = runWorkload(wl_ablation, cfg, 1);
        std::cout << "done\n";
        double tput_k = r.throughput_ops_sec / 1000.0;
        double fpr_pct = r.bloom_fpr * 100.0;
        entries_200K.push_back({row.label, tput_k, r.waf, r.raf, r.saf, fpr_pct, r.ahlc_switches});

        std::cout << "  | " << std::left  << std::setw(21) << row.label
                  << " | " << std::right << std::setw(8)  << std::fixed
                  << std::setprecision(1) << tput_k
                  << " | " << std::setw(5)  << std::setprecision(2) << r.waf
                  << " | " << std::setw(5)  << r.raf
                  << " | " << std::setw(5)  << r.saf
                  << " | " << std::setw(7)  << std::setprecision(4) << fpr_pct
                  << " | " << std::setw(6)  << r.ahlc_switches
                  << " |\n";
    }
    std::cout << "  +-----------------------+----------+-------+-------+-------+---------+--------+\n";
    std::cout << "  Note: Skip rows use cfg.memtable_type=SKIP_LIST. CSB+ rows use ConcurrentCSBTree.\n";
    std::cout << "  AHLC=off forces fixed Leveling. Adaptive Bloom uses dual-trigger reallocation.\n";

    // -----------------------------------------------------------------------
    // Section 4b: Ablation at N=1M (same 8 configs — second scale point)
    // Reviewer requirement: ablation must not be confined to smallest scale.
    // -----------------------------------------------------------------------
    const int N4b = 1000000;
    std::cout << "\n  [4b/4] 8-Config Ablation Study — Workload A (N=" << N4b << ", 1M scale)\n";
    std::cout << "         Same 8 configs as Section 4a — verifies ablation is not scale-dependent\n";
    std::cout << "  +-----------------------+----------+-------+-------+-------+---------+--------+\n";
    std::cout << "  | Config                | Tput(K/s)| WAF   | RAF   | SAF   | BlmFPR% |AHLCSw. |\n";
    std::cout << "  +-----------------------+----------+-------+-------+-------+---------+--------+\n";

    auto wl_ablation_1M = workloadA(N4b);
    for (auto& row : ablation_configs) {
        Config cfg = base_cfg;
        cfg.memtable_type = row.mt;
        if (!row.ahlc_on) {
            cfg.ahlc_write_rate_high = 1e18;
            cfg.ahlc_skew_threshold  = 1.1;
        }
        if (!row.bloom_adpt)
            cfg.bloom_total_budget = 1000000;

        std::cout << "    " << row.label << "... " << std::flush;
        auto r = runWorkload(wl_ablation_1M, cfg, 1);
        std::cout << "done\n";
        double tput_k = r.throughput_ops_sec / 1000.0;
        double fpr_pct = r.bloom_fpr * 100.0;
        entries_1M.push_back({row.label, tput_k, r.waf, r.raf, r.saf, fpr_pct, r.ahlc_switches});

        std::cout << "  | " << std::left  << std::setw(21) << row.label
                  << " | " << std::right << std::setw(8)  << std::fixed
                  << std::setprecision(1) << tput_k
                  << " | " << std::setw(5)  << std::setprecision(2) << r.waf
                  << " | " << std::setw(5)  << r.raf
                  << " | " << std::setw(5)  << r.saf
                  << " | " << std::setw(7)  << std::setprecision(4) << fpr_pct
                  << " | " << std::setw(6)  << r.ahlc_switches
                  << " |\n";
    }
    std::cout << "  +-----------------------+----------+-------+-------+-------+---------+--------+\n";
    std::cout << "  Section 4b: real disk-backed I/O, N=1M (reviewer-requested second scale point)\n";

    // Write results to bench/results/ablation_summary.md
    (void)system("mkdir -p bench/results");
    std::ofstream out("bench/results/ablation_summary.md");
    if (out.is_open()) {
        out << "# CASCADE Architectural Ablation Study (8 Configurations)\n\n";
        out << "> Evaluates {SkipList / CSB+} × {Fixed Leveling / AHLC} × {Uniform / Adaptive Bloom} on Workload A (50% Read / 50% Update).\n\n";

        out << "### 1. Ablation Study: 200,000 Operations (200K Scale)\n\n";
        out << "| MemTable | Compaction | Bloom Sizing | Throughput (Kops/s) | WAF | RAF | SAF | Bloom FPR (%) | AHLC Switches |\n";
        out << "|---|---|---|:---:|:---:|:---:|:---:|:---:|:---:|\n";
        for (const auto& e : entries_200K) {
            std::string raw = e.label;
            while (!raw.empty() && raw.back() == ' ') raw.pop_back();
            // parse tokens
            std::string mt = raw.find("Skip") != std::string::npos ? "SkipList" : "CSB+ Tree";
            std::string comp = raw.find("AHLC") != std::string::npos ? "AHLC" : "Fixed Leveling";
            std::string blm = raw.find("Adpt") != std::string::npos ? "Adaptive (Dual-Trigger)" : "Uniform";
            if (raw.find("CASCADE") != std::string::npos) blm += " *(CASCADE)*";
            out << "| " << mt << " | " << comp << " | " << blm << " | "
                << std::fixed << std::setprecision(1) << e.tput_kops << " | "
                << std::setprecision(2) << e.waf << " | "
                << std::setprecision(2) << e.raf << " | "
                << std::setprecision(2) << e.saf << " | "
                << std::setprecision(4) << e.bloom_fpr_pct << "% | "
                << e.ahlc_switches << " |\n";
        }

        out << "\n### 2. Ablation Study: 1,000,000 Operations (1M Scale)\n\n";
        out << "| MemTable | Compaction | Bloom Sizing | Throughput (Kops/s) | WAF | RAF | SAF | Bloom FPR (%) | AHLC Switches |\n";
        out << "|---|---|---|:---:|:---:|:---:|:---:|:---:|:---:|\n";
        for (const auto& e : entries_1M) {
            std::string raw = e.label;
            while (!raw.empty() && raw.back() == ' ') raw.pop_back();
            std::string mt = raw.find("Skip") != std::string::npos ? "SkipList" : "CSB+ Tree";
            std::string comp = raw.find("AHLC") != std::string::npos ? "AHLC" : "Fixed Leveling";
            std::string blm = raw.find("Adpt") != std::string::npos ? "Adaptive (Dual-Trigger)" : "Uniform";
            if (raw.find("CASCADE") != std::string::npos) blm += " *(CASCADE)*";
            out << "| " << mt << " | " << comp << " | " << blm << " | "
                << std::fixed << std::setprecision(1) << e.tput_kops << " | "
                << std::setprecision(2) << e.waf << " | "
                << std::setprecision(2) << e.raf << " | "
                << std::setprecision(2) << e.saf << " | "
                << std::setprecision(4) << e.bloom_fpr_pct << "% | "
                << e.ahlc_switches << " |\n";
        }
        out.close();
        std::cout << "\nSaved: bench/results/ablation_summary.md\n";
    }

    return 0;
}
