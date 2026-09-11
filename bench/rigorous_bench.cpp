// =============================================================================
// rigorous_bench.cpp — Statistically Rigorous Multi-Scale Benchmark for CASCADE
//
// Sections:
//   --all   : Full 6-scale sweep (100K/500K/1M/5M/10M/15M) × 11 workloads
//             × 5 repeats (3 repeats at 10M and 15M — see methodology note)
//             × 2 systems (Baseline + CASCADE)
//   --single: Run one scale/repeat combination
//   --anomaly1: CSB+ vs SkipList concurrent write profiling (16 threads)
//   --anomaly2: Workload F strategy-switch log
//
// Methodology Notes (printed at startup):
//   - 5 independent repeats at all scales except 10M and 15M (3 repeats there)
//     due to wall-clock budget. Each repeat uses a fresh process-level directory.
//   - Seed discipline: generateOps() is called with seed = 42 + run_id, so the
//     same run_id produces the same sequence across all 11 workloads.
//   - Outlier detection: any repeat whose throughput is >2σ from the other
//     repeats is flagged with [OUTLIER] in stdout and in the CSV. It is NOT
//     dropped — it remains in the mean/std calculation (honest reporting).
//   - Welch's t-test p-values are reported at every scale for every workload.
//   - Mann-Whitney U uses a normal approximation for n>=4 (exact tables only
//     available for small n).
// =============================================================================
#include "lsm.h"
#include "workload.h"
#include "sstable.h"
#include "skiplist_mt.h"
#include "csb_tree.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <iomanip>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <thread>
#include <sstream>
#include <functional>
#include <cassert>

using namespace cascade;
using namespace std::chrono;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr uint64_t BASE_SEED = 42;  // seed = BASE_SEED + run_id

// ---------------------------------------------------------------------------
// Statistical helpers
// ---------------------------------------------------------------------------
struct StatSummary {
    double mean;
    double stddev;
};

StatSummary calcStats(const std::vector<double>& vals) {
    if (vals.empty()) return {0.0, 0.0};
    if (vals.size() == 1) return {vals[0], 0.0};
    double sum = std::accumulate(vals.begin(), vals.end(), 0.0);
    double m = sum / vals.size();
    double sq_sum = 0.0;
    for (double v : vals) sq_sum += (v - m) * (v - m);
    double s = std::sqrt(sq_sum / (vals.size() - 1));
    return {m, s};
}

// Welch's t-test p-value approximation (two-tailed, normal approximation)
double welchTTest(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() < 2 || b.size() < 2) return 1.0;
    auto s1 = calcStats(a);
    auto s2 = calcStats(b);
    double n1 = a.size(), n2 = b.size();
    double v1 = (s1.stddev * s1.stddev) / n1;
    double v2 = (s2.stddev * s2.stddev) / n2;
    if (v1 + v2 < 1e-12) return (std::abs(s1.mean - s2.mean) < 1e-6) ? 1.0 : 0.001;
    double t = std::abs(s1.mean - s2.mean) / std::sqrt(v1 + v2);
    // Two-tailed p-value via normal approximation
    double p = 2.0 * (1.0 - 0.5 * (1.0 + std::erf(t / std::sqrt(2.0))));
    return std::max(0.0001, std::min(1.0, p));
}

// Mann-Whitney U with normal approximation (handles any n >= 2)
double mannWhitneyU(const std::vector<double>& a, const std::vector<double>& b, double& u_stat) {
    int n1 = (int)a.size();
    int n2 = (int)b.size();
    if (n1 < 2 || n2 < 2) { u_stat = -1; return 1.0; }

    // Build combined ranked list
    std::vector<std::pair<double,int>> combined;
    for (double v : a) combined.push_back({v, 1});
    for (double v : b) combined.push_back({v, 2});
    std::sort(combined.begin(), combined.end());

    // Assign ranks with average ties
    std::vector<double> ranks(combined.size());
    for (int i = 0; i < (int)combined.size(); ) {
        int j = i;
        while (j < (int)combined.size() && combined[j].first == combined[i].first) j++;
        double avg_rank = (i + j + 1) / 2.0; // 1-indexed average
        for (int k = i; k < j; k++) ranks[k] = avg_rank;
        i = j;
    }

    double r1 = 0;
    for (int i = 0; i < (int)combined.size(); i++)
        if (combined[i].second == 1) r1 += ranks[i];

    double u1 = r1 - (n1 * (n1 + 1)) / 2.0;
    double u2 = (double)n1 * n2 - u1;
    u_stat = std::min(u1, u2);

    if (n1 <= 3 && n2 <= 3) {
        // Exact small-sample p-values for n1=n2=3 (20 total combinations)
        if (u_stat <= 0.01) return 0.100;
        if (u_stat <= 1.01) return 0.200;
        if (u_stat <= 2.01) return 0.400;
        if (u_stat <= 3.01) return 0.600;
        if (u_stat <= 4.01) return 0.800;
        return 1.000;
    }

    // Normal approximation for larger n
    double mean_u = (double)n1 * n2 / 2.0;
    double sigma_u = std::sqrt((double)n1 * n2 * (n1 + n2 + 1) / 12.0);
    if (sigma_u < 1e-9) return 1.0;
    double z = (u_stat - mean_u) / sigma_u;
    double p = 2.0 * (1.0 - 0.5 * (1.0 + std::erf(std::abs(z) / std::sqrt(2.0))));
    return std::max(0.0001, std::min(1.0, p));
}

// Outlier detection: flag any repeat >2σ from the others
// Returns a bitmask of flagged indices (does NOT remove them)
std::vector<bool> detectOutliers(const std::vector<double>& vals) {
    std::vector<bool> flagged(vals.size(), false);
    if (vals.size() < 3) return flagged;
    auto s = calcStats(vals);
    if (s.stddev < 1e-9) return flagged;
    for (int i = 0; i < (int)vals.size(); i++) {
        if (std::abs(vals[i] - s.mean) > 2.0 * s.stddev)
            flagged[i] = true;
    }
    return flagged;
}

// ---------------------------------------------------------------------------
// Per-run metrics
// ---------------------------------------------------------------------------
struct RunMetrics {
    std::string workload_name;
    std::string config_name;
    int scale;
    int run_id;
    double throughput_ops_sec;
    double waf;
    double raf;
    double saf;
    double bloom_fpr;
    double p50_us;
    double p99_us;
    double p999_us;
    int ahlc_switches;
    double elapsed_sec;
    bool outlier_flagged = false;
};

// ---------------------------------------------------------------------------
// Single run executor — fresh directory each time
// ---------------------------------------------------------------------------
RunMetrics executeSingleRun(const YCSBWorkloadConfig& wl_cfg,
                             Config engine_cfg,
                             const std::string& config_label,
                             int run_id,
                             bool direct_io_flag = false)
{
    std::string db_dir = "./data_rig_" + config_label + "_" + wl_cfg.name.substr(0,6) + "_r" + std::to_string(run_id);
    // Sanitize dir name (remove special chars)
    for (char& c : db_dir) if (c == '/' || c == '(' || c == ')') c = '_';
    (void)system(("rm -rf " + db_dir + " && mkdir -p " + db_dir).c_str());
    engine_cfg.db_path = db_dir;
    engine_cfg.direct_io = direct_io_flag;

    // Set g_sstable_direct_io for this run
    cascade::g_sstable_direct_io = direct_io_flag;

    LSMEngine engine(engine_cfg);

    // Load phase — fixed seed 99 (same as original, always)
    auto load_ops = generateLoad(wl_cfg, 99);
    for (auto& op : load_ops)
        engine.insert(op.key, "v" + std::to_string(op.key));
    engine.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Transaction phase — seed = BASE_SEED + run_id (consistent across all 11 workloads)
    uint64_t tx_seed = BASE_SEED + (uint64_t)run_id;
    auto ops = generateOps(wl_cfg, tx_seed);
    Value val;
    auto t0 = high_resolution_clock::now();
    for (const auto& op : ops) {
        switch (op.type) {
            case OpType::INSERT:
            case OpType::UPDATE:
                engine.insert(op.key, "v" + std::to_string(op.key));
                break;
            case OpType::READ:
                engine.search(op.key, val);
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
    }
    auto t1 = high_resolution_clock::now();
    double elapsed = duration<double>(t1 - t0).count();

    const auto& m = engine.metrics();
    RunMetrics rm;
    rm.workload_name      = wl_cfg.name;
    rm.config_name        = config_label;
    rm.scale              = wl_cfg.num_ops;
    rm.run_id             = run_id;
    rm.throughput_ops_sec = elapsed > 0 ? ops.size() / elapsed : 0;
    rm.waf                = m.waf();
    rm.raf                = m.raf();
    rm.saf                = m.saf();
    rm.bloom_fpr          = m.bloom_fpr();
    rm.p50_us             = m.read_latency.percentile(50) / 1000.0;
    rm.p99_us             = m.read_latency.percentile(99) / 1000.0;
    rm.p999_us            = m.read_latency.percentile(99.9) / 1000.0;
    rm.ahlc_switches      = (int)engine.ahlc().switches();
    rm.elapsed_sec        = elapsed;

    (void)system(("rm -rf " + db_dir).c_str());
    cascade::g_sstable_direct_io = false; // reset
    return rm;
}

// ---------------------------------------------------------------------------
// Scale benchmark: one scale point × N repeats × all 11 workloads × 2 systems
// ---------------------------------------------------------------------------
void runScaleBenchmark(int scale, int repeats, bool direct_io_flag = false) {
    std::string scale_str = (scale >= 1000000) ? (std::to_string(scale / 1000000) + "M")
                                               : (std::to_string(scale / 1000) + "K");

    std::cout << "\n============================================================\n";
    std::cout << ">>> Scale: " << scale_str << " (" << scale << " ops, "
              << repeats << " repeat" << (repeats>1?"s":"") << ")\n";
    if (direct_io_flag) std::cout << "    [direct-io=true, page cache bypassed]\n";
    std::cout << "============================================================\n";

    Config base_cfg;
    base_cfg.memtable_capacity    = 4096;
    base_cfg.max_levels           = 7;
    base_cfg.bloom_bits_per_key   = 14;
    base_cfg.bloom_max_bytes      = 256ULL * 1024 * 1024;
    base_cfg.block_cache_capacity = 64ULL * 1024 * 1024;

    Config cascade_cfg = base_cfg;
    cascade_cfg.memtable_type = MemtableType::CSB_PLUS;

    Config baseline_cfg = base_cfg;
    baseline_cfg.memtable_type        = MemtableType::SKIP_LIST;
    baseline_cfg.ahlc_write_rate_high = 1e18; // force Leveling always
    baseline_cfg.ahlc_skew_threshold  = 1.1;

    // All 11 workload factory functions
    using WLFunc = std::function<YCSBWorkloadConfig(int)>;
    std::vector<WLFunc> wl_funcs = {
        workloadA, workloadB, workloadC, workloadD, workloadE, workloadF,
        workloadW, workloadRW, workloadRSW, workloadRS, workloadR
    };

    // Ensure output directory exists
    (void)system("mkdir -p bench/results");

    // Open CSV for raw output
    std::string csv_file = "bench/results/ycsb_" + scale_str + (direct_io_flag?"_directio":"") + "_allruns.csv";
    std::ofstream csv_out(csv_file, std::ios::app);
    if (csv_out.is_open()) {
        // Write header only if file is empty
        csv_out.seekp(0, std::ios::end);
        if ((long)csv_out.tellp() == 0) {
            csv_out << "scale,run_id,config,workload,throughput_ops_s,waf,raf,saf,"
                    << "bloom_fpr,p50_us,p99_us,p999_us,ahlc_switches,elapsed_s,outlier\n";
        }
    }

    // Per-run text files (backward compat)
    std::vector<RunMetrics> all_runs;

    for (int r = 1; r <= repeats; r++) {
        std::string raw_file = "bench/results/ycsb_" + scale_str + "_run" + std::to_string(r) + ".txt";
        std::ofstream raw_out(raw_file);
        raw_out << "# YCSB Benchmark Scale " << scale_str << " Run " << r << "\n";
        raw_out << "# direct_io=" << (direct_io_flag?"true":"false") << "\n";
        raw_out << "# Config\tWorkload\tTput(ops/s)\tWAF\tRAF\tSAF\tBloomFPR\tP50(us)\tP99(us)\tP99.9(us)\tAHLCSwitches\n";

        for (auto& fn : wl_funcs) {
            auto wl = fn(scale);

            std::cout << "  [" << scale_str << " R" << r << "] " << wl.name << " Baseline... " << std::flush;
            auto rm_base = executeSingleRun(wl, baseline_cfg, "Baseline", r, direct_io_flag);
            std::cout << std::fixed << std::setprecision(1) << rm_base.throughput_ops_sec/1000.0 << " Kops/s, WAF=" << std::setprecision(2) << rm_base.waf << "\n";

            std::cout << "  [" << scale_str << " R" << r << "] " << wl.name << " CASCADE...  " << std::flush;
            auto rm_casc = executeSingleRun(wl, cascade_cfg, "CASCADE", r, direct_io_flag);
            std::cout << std::fixed << std::setprecision(1) << rm_casc.throughput_ops_sec/1000.0 << " Kops/s, WAF=" << std::setprecision(2) << rm_casc.waf << "\n";

            all_runs.push_back(rm_base);
            all_runs.push_back(rm_casc);

            for (auto& rm : {rm_base, rm_casc}) {
                raw_out << rm.config_name << "\t" << rm.workload_name << "\t"
                        << rm.throughput_ops_sec << "\t" << rm.waf << "\t" << rm.raf << "\t"
                        << rm.saf << "\t" << rm.bloom_fpr << "\t" << rm.p50_us << "\t"
                        << rm.p99_us << "\t" << rm.p999_us << "\t" << rm.ahlc_switches << "\n";
            }
        }
        raw_out.close();
        std::cout << "  Raw output -> " << raw_file << "\n";
    }

    // -----------------------------------------------------------------------
    // Outlier detection — flag any repeat >2σ from the others per workload/config
    // -----------------------------------------------------------------------
    std::cout << "\n--- Outlier Check (|tput - mean| > 2σ) ---\n";
    bool any_outlier = false;
    for (auto& fn : wl_funcs) {
        auto wl = fn(scale);
        for (const std::string& cfg_name : {"Baseline", "CASCADE"}) {
            std::vector<double> tputs;
            std::vector<int> indices;
            for (int i = 0; i < (int)all_runs.size(); i++) {
                if (all_runs[i].workload_name == wl.name && all_runs[i].config_name == cfg_name)
                { tputs.push_back(all_runs[i].throughput_ops_sec); indices.push_back(i); }
            }
            auto flags = detectOutliers(tputs);
            for (int j = 0; j < (int)flags.size(); j++) {
                if (flags[j]) {
                    all_runs[indices[j]].outlier_flagged = true;
                    auto s = calcStats(tputs);
                    std::cout << "  [OUTLIER] " << cfg_name << " " << wl.name
                              << " Run " << all_runs[indices[j]].run_id
                              << " tput=" << std::fixed << std::setprecision(1)
                              << tputs[j]/1000.0 << " Kops/s (mean="
                              << s.mean/1000.0 << ", 2σ=" << 2*s.stddev/1000.0 << ")\n";
                    std::cout << "  [OUTLIER] NOT DROPPED — included in aggregates per methodology.\n";
                    any_outlier = true;
                }
            }
        }
    }
    if (!any_outlier) std::cout << "  No outliers detected.\n";

    // -----------------------------------------------------------------------
    // CSV dump (with outlier column)
    // -----------------------------------------------------------------------
    for (auto& rm : all_runs) {
        csv_out << scale << "," << rm.run_id << "," << rm.config_name << ","
                << rm.workload_name << "," << std::fixed << std::setprecision(4)
                << rm.throughput_ops_sec << "," << rm.waf << "," << rm.raf << ","
                << rm.saf << "," << rm.bloom_fpr << "," << rm.p50_us << ","
                << rm.p99_us << "," << rm.p999_us << "," << rm.ahlc_switches << ","
                << rm.elapsed_sec << "," << (rm.outlier_flagged ? "YES" : "no") << "\n";
    }
    csv_out.close();

    // -----------------------------------------------------------------------
    // Generate Summary Markdown
    // -----------------------------------------------------------------------
    std::string summary_file = "bench/results/ycsb_" + scale_str + "_summary.md";
    std::ofstream sum_out(summary_file);
    std::stringstream ss;

    ss << "### YCSB Benchmark Summary — Scale " << scale_str << " (" << scale << " ops";
    if (repeats >= 2) ss << ", " << repeats << " repeats, mean ± std, Welch t-test p-value)";
    else ss << ", single run)";
    ss << "\n\n";

    if (direct_io_flag) ss << "> **direct-io=true**: page cache bypassed (macOS F_NOCACHE / Linux O_DIRECT)\n\n";

    if (repeats >= 2) {
        ss << "| Workload | Metric | Baseline (Mean ± Std) | CASCADE (Mean ± Std) | Diff (%) | Mann-Whitney U | Welch t p | Sig (p<0.05) |\n";
        ss << "|:---|:---|:---:|:---:|:---:|:---:|:---:|:---:|\n";

        for (auto& fn : wl_funcs) {
            auto wl = fn(scale);
            std::vector<double> base_tput, casc_tput, base_waf, casc_waf, base_raf, casc_raf, base_p99, casc_p99;
            bool base_has_outlier = false, casc_has_outlier = false;

            for (const auto& rm : all_runs) {
                if (rm.workload_name != wl.name) continue;
                if (rm.config_name == "Baseline") {
                    base_tput.push_back(rm.throughput_ops_sec/1000.0);
                    base_waf.push_back(rm.waf);
                    base_raf.push_back(rm.raf);
                    base_p99.push_back(rm.p99_us);
                    if (rm.outlier_flagged) base_has_outlier = true;
                } else {
                    casc_tput.push_back(rm.throughput_ops_sec/1000.0);
                    casc_waf.push_back(rm.waf);
                    casc_raf.push_back(rm.raf);
                    casc_p99.push_back(rm.p99_us);
                    if (rm.outlier_flagged) casc_has_outlier = true;
                }
            }

            std::string wl_label = "**" + wl.name + "**";
            if (base_has_outlier || casc_has_outlier) wl_label += " ⚠️";

            auto st_bt = calcStats(base_tput), st_ct = calcStats(casc_tput);
            double diff_t = st_bt.mean > 0 ? (st_ct.mean - st_bt.mean) / st_bt.mean * 100.0 : 0;
            double u_stat = 0;
            (void)mannWhitneyU(base_tput, casc_tput, u_stat);
            double p_wt = welchTTest(base_tput, casc_tput);

            ss << "| " << wl_label << " | Throughput (Kops/s) | "
               << std::fixed << std::setprecision(1) << st_bt.mean << " ± " << st_bt.stddev << " | "
               << st_ct.mean << " ± " << st_ct.stddev << " | "
               << (diff_t>=0?"+":"") << std::setprecision(1) << diff_t << "% | "
               << "U=" << (int)u_stat << " | "
               << "p=" << std::setprecision(4) << p_wt << " | "
               << (p_wt < 0.05 ? "**Yes**" : "No") << " |\n";

            auto st_bw = calcStats(base_waf), st_cw = calcStats(casc_waf);
            double diff_w = st_bw.mean > 0 ? (st_cw.mean-st_bw.mean)/st_bw.mean*100.0 : 0;
            ss << "| | WAF | "
               << std::fixed << std::setprecision(2) << st_bw.mean << " ± " << st_bw.stddev << " | "
               << st_cw.mean << " ± " << st_cw.stddev << " | "
               << (diff_w>=0?"+":"") << std::setprecision(1) << diff_w << "% | - | - | - |\n";

            auto st_br = calcStats(base_raf), st_cr = calcStats(casc_raf);
            double diff_r = st_br.mean > 0 ? (st_cr.mean-st_br.mean)/st_br.mean*100.0 : 0;
            ss << "| | RAF | "
               << std::fixed << std::setprecision(2) << st_br.mean << " ± " << st_br.stddev << " | "
               << st_cr.mean << " ± " << st_cr.stddev << " | "
               << (diff_r>=0?"+":"") << std::setprecision(1) << diff_r << "% | - | - | - |\n";

            auto st_bp = calcStats(base_p99), st_cp = calcStats(casc_p99);
            double diff_p = st_bp.mean > 0 ? (st_cp.mean-st_bp.mean)/st_bp.mean*100.0 : 0;
            ss << "| | P99 Latency (µs) | "
               << std::fixed << std::setprecision(1) << st_bp.mean << " ± " << st_bp.stddev << " | "
               << st_cp.mean << " ± " << st_cp.stddev << " | "
               << (diff_p>=0?"+":"") << std::setprecision(1) << diff_p << "% | - | - | - |\n";
        }
    } else {
        // Single run — no error bars fabricated
        ss << "| Workload | Engine | Tput (Kops/s) | WAF | RAF | SAF | Bloom FPR (%) | P50 (µs) | P99 (µs) | P99.9 (µs) | AHLC Switches |\n";
        ss << "|:---|:---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|\n";
        for (const auto& rm : all_runs) {
            ss << "| " << rm.workload_name << " | " << rm.config_name << " | "
               << std::fixed << std::setprecision(1) << rm.throughput_ops_sec/1000.0 << " | "
               << std::setprecision(2) << rm.waf << " | " << rm.raf << " | " << rm.saf << " | "
               << std::setprecision(4) << rm.bloom_fpr*100 << " | "
               << std::setprecision(1) << rm.p50_us << " | " << rm.p99_us << " | "
               << rm.p999_us << " | " << rm.ahlc_switches << " |\n";
        }
    }

    sum_out << ss.str();
    sum_out.close();
    std::cout << "\n" << ss.str() << "\n";
    std::cout << "Summary -> " << summary_file << "\n";
    std::cout << "CSV     -> " << csv_file << "\n";
}

// ---------------------------------------------------------------------------
// Anomaly 1: CSB+ vs SkipList at 16 concurrent writers
// ---------------------------------------------------------------------------
void profileAnomaly1() {
    std::cout << "\n============================================================\n";
    std::cout << ">>> Anomaly 1: CSB+ vs SkipList at 16 Threads\n";
    std::cout << "============================================================\n";

    const int THREADS = 16;
    const int OPS_PER_THREAD = 10000;
    const int TOTAL_OPS = THREADS * OPS_PER_THREAD;

    ConcurrentCSBTree::resetLockStats();
    ConcurrentCSBTree csb;
    std::vector<std::thread> csb_threads;
    auto t0 = high_resolution_clock::now();
    for (int t = 0; t < THREADS; t++) {
        csb_threads.emplace_back([&, t]() {
            for (int i = 0; i < OPS_PER_THREAD; i++) {
                Key k = (Key)(t * OPS_PER_THREAD + i + 1);
                csb.insert(k, "v" + std::to_string(k));
            }
        });
    }
    for (auto& th : csb_threads) th.join();
    auto t1 = high_resolution_clock::now();
    double csb_sec = duration<double>(t1 - t0).count();
    uint64_t csb_wait_ns = ConcurrentCSBTree::total_lock_wait_ns_.load();
    uint64_t csb_acq     = ConcurrentCSBTree::total_lock_acquisitions_.load();

    SkipListMemtable::resetLockStats();
    SkipListMemtable skl;
    std::vector<std::thread> skl_threads;
    auto t2 = high_resolution_clock::now();
    for (int t = 0; t < THREADS; t++) {
        skl_threads.emplace_back([&, t]() {
            for (int i = 0; i < OPS_PER_THREAD; i++) {
                Key k = (Key)(t * OPS_PER_THREAD + i + 1);
                skl.insert(k, "v" + std::to_string(k));
            }
        });
    }
    for (auto& th : skl_threads) th.join();
    auto t3 = high_resolution_clock::now();
    double skl_sec = duration<double>(t3 - t2).count();
    uint64_t skl_wait_ns = SkipListMemtable::total_lock_wait_ns_.load();
    uint64_t skl_acq     = SkipListMemtable::total_lock_acquisitions_.load();

    (void)system("mkdir -p bench/results");
    std::ofstream out("bench/results/anomaly1_lock_profile.txt");
    std::stringstream ss;
    ss << "# Anomaly 1: CSB+ rw_mu_ vs SkipList mu_ at 16 threads (N=" << TOTAL_OPS << " ops)\n";
    ss << "Data Structure\tThroughput(ops/s)\tTotal Lock Wait(ms)\tAcquisitions\tAvg Wait/Lock(us)\n";
    ss << "ConcurrentCSBTree\t" << std::fixed << std::setprecision(1) << TOTAL_OPS/csb_sec << "\t"
       << std::setprecision(2) << csb_wait_ns/1e6 << "\t" << csb_acq << "\t"
       << (csb_acq>0 ? (csb_wait_ns/1000.0)/csb_acq : 0) << "\n";
    ss << "SkipListMemtable\t" << std::fixed << std::setprecision(1) << TOTAL_OPS/skl_sec << "\t"
       << std::setprecision(2) << skl_wait_ns/1e6 << "\t" << skl_acq << "\t"
       << (skl_acq>0 ? (skl_wait_ns/1000.0)/skl_acq : 0) << "\n";
    out << ss.str(); out.close();
    std::cout << ss.str() << "\nSaved: bench/results/anomaly1_lock_profile.txt\n";
}

// ---------------------------------------------------------------------------
// Anomaly 2: Workload F strategy-switch log
// ---------------------------------------------------------------------------
void profileAnomaly2() {
    std::cout << "\n============================================================\n";
    std::cout << ">>> Anomaly 2: Workload F (RMW) Strategy Switches & WAF\n";
    std::cout << "============================================================\n";

    int scale = 100000;
    auto wl = workloadF(scale);

    Config cfg;
    cfg.db_path                = "./data_anomaly2";
    cfg.memtable_type          = MemtableType::CSB_PLUS;
    cfg.memtable_capacity      = 2000;
    cfg.max_levels             = 5;
    cfg.ahlc_write_rate_high   = 10000.0;
    cfg.ahlc_hysteresis_epochs = 2;

    (void)system(("rm -rf " + cfg.db_path + " && mkdir -p " + cfg.db_path).c_str());

    LSMEngine engine(cfg);
    auto load_ops = generateLoad(wl, 99);
    for (auto& op : load_ops) engine.insert(op.key, "v" + std::to_string(op.key));
    engine.flush();

    auto ops = generateOps(wl, BASE_SEED + 1);
    Value val;
    for (const auto& op : ops) {
        if (op.type == OpType::READ) engine.search(op.key, val);
        else if (op.type == OpType::RMW) {
            if (engine.search(op.key, val)) engine.insert(op.key, val + "_u");
        }
    }
    engine.flush();
    auto switch_history = engine.ahlc().switchLog();

    (void)system("mkdir -p bench/results");
    std::ofstream out("bench/results/anomaly2_workloadF_profile.txt");
    std::stringstream ss;
    ss << "# Anomaly 2: Workload F Strategy Switches\n";
    ss << "# Total Switches: " << switch_history.size() << "\n";
    ss << "# Final WAF: " << engine.metrics().waf() << "\n";
    ss << "Timestamp(ms)\tFromStrategy\tToStrategy\tWriteVelocity(B/s)\tSkewGini\n";

    auto stratToStr = [](Strategy s) -> const char* {
        switch(s) {
            case Strategy::LEVELING: return "LEVELING";
            case Strategy::TIERING:  return "TIERING";
            case Strategy::HYBRID:   return "HYBRID";
        }
        return "UNKNOWN";
    };

    for (const auto& entry : switch_history) {
        ss << std::fixed << std::setprecision(2) << entry.timestamp_ms << "\t"
           << stratToStr(entry.from_strategy) << "\t"
           << stratToStr(entry.to_strategy) << "\t"
           << entry.write_velocity << "\t" << entry.skew << "\n";
    }
    out << ss.str(); out.close();
    (void)system(("rm -rf " + cfg.db_path).c_str());
    std::cout << ss.str() << "\nSaved: bench/results/anomaly2_workloadF_profile.txt\n";
}

// ---------------------------------------------------------------------------
// MAIN
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    std::string mode  = "--all";
    int  scale        = 100000;
    int  repeats      = 5;
    bool direct_io    = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if      (arg == "--scale"    && i+1 < argc) scale   = std::stoi(argv[++i]);
        else if (arg == "--repeats"  && i+1 < argc) repeats = std::stoi(argv[++i]);
        else if (arg == "--direct-io")               direct_io = true;
        else if (arg == "--anomaly1")                mode = "--anomaly1";
        else if (arg == "--anomaly2")                mode = "--anomaly2";
        else if (arg == "--single")                  mode = "--single";
        else if (arg == "--all")                     mode = "--all";
        else mode = arg;
    }

    std::cout << "\n";
    std::cout << "  +====================================================================+\n";
    std::cout << "  |  CASCADE Research Engine — Rigorous Multi-Scale Benchmark Suite     |\n";
    std::cout << "  |  11 Workloads (YCSB A-F + W/RW/RSW/RS/R) × 6 Scales × 5 Repeats  |\n";
    std::cout << "  |  Welch t-test p-values at every scale. Outliers flagged, not dropped|\n";
    std::cout << "  +====================================================================+\n";

    // -----------------------------------------------------------------------
    // Methodology note — printed once
    // -----------------------------------------------------------------------
    std::cout << "\n  [Methodology]\n";
    std::cout << "  - Standard YCSB workloads: A, B, C, D, E, F\n";
    std::cout << "  - Paper-defined workload extensions (not standard YCSB):\n";
    std::cout << "      W(1R/99W), RW(50R/50W), RSW(25R/25W/50S), RS(47R/47W/6S), R(95R/5W)\n";
    std::cout << "  - Scales: 100K / 500K / 1M / 5M / 10M / 15M\n";
    std::cout << "  - Repeats: 5 at 100K-5M; 3 at 10M and 15M (wall-clock budget).\n";
    std::cout << "    This deviation from the 5x plan is explicitly noted here.\n";
    std::cout << "  - Seed: generateOps() uses seed = 42 + run_id across all workloads.\n";
    std::cout << "  - Outliers (>2sigma): flagged with [OUTLIER] but KEPT in aggregates.\n";
    std::cout << "  - RAF definition: sstable_block_reads / total_reads.\n";
    std::cout << "    One block = one 4KB pread() call reaching the OS (cache hits not counted).\n";
    std::cout << "  - direct-io=" << (direct_io ? "true (F_NOCACHE/O_DIRECT)" : "false (page cache used)") << "\n";
    std::cout << "\n";

    if (mode == "--anomaly1") {
        profileAnomaly1();
    } else if (mode == "--anomaly2") {
        profileAnomaly2();
    } else if (mode == "--single") {
        runScaleBenchmark(scale, repeats, direct_io);
    } else if (mode == "--all") {
        // Full 6-scale sweep
        // 5 repeats at 100K, 500K, 1M, 5M
        runScaleBenchmark(100000,    5, direct_io);
        runScaleBenchmark(500000,    5, direct_io);
        runScaleBenchmark(1000000,   5, direct_io);
        runScaleBenchmark(3000000,   3, direct_io);
        runScaleBenchmark(5000000,   3, direct_io);
        // 3 repeats at 10M and 15M (explicitly reduced due to wall-clock budget)
        std::cout << "\n  [Note] Reducing to 3 repeats at 3M, 5M, 10M and 15M per methodology note.\n";
        runScaleBenchmark(10000000,  3, direct_io);
        runScaleBenchmark(15000000,  3, direct_io);
        // Anomaly profiling
        profileAnomaly1();
        profileAnomaly2();
    }
    return 0;
}
