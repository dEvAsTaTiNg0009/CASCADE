#include "lsm.h"
#include "workload.h"
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

using namespace cascade;
using namespace std::chrono;

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
};

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

// Exact Mann-Whitney U test for n1=3, n2=3
double mannWhitneyU(const std::vector<double>& a, const std::vector<double>& b, double& u_stat) {
    int n1 = (int)a.size();
    int n2 = (int)b.size();
    if (n1 != 3 || n2 != 3) {
        u_stat = -1;
        return 1.0;
    }
    std::vector<std::pair<double, int>> combined;
    for (double v : a) combined.push_back({v, 1});
    for (double v : b) combined.push_back({v, 2});
    std::sort(combined.begin(), combined.end());

    double r1 = 0;
    for (int i = 0; i < 6; i++) {
        if (combined[i].second == 1) r1 += (i + 1);
    }
    double u1 = r1 - (n1 * (n1 + 1)) / 2.0;
    double u2 = n1 * n2 - u1;
    u_stat = std::min(u1, u2);

    // Exact two-tailed p-value for n1=3, n2=3 (20 total combinations)
    if (u_stat <= 0.01) return 0.100;
    if (u_stat <= 1.01) return 0.200;
    if (u_stat <= 2.01) return 0.400;
    if (u_stat <= 3.01) return 0.600;
    if (u_stat <= 4.01) return 0.800;
    return 1.000;
}

// Welch's t-test p-value approximation
double welchTTest(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() < 2 || b.size() < 2) return 1.0;
    auto s1 = calcStats(a);
    auto s2 = calcStats(b);
    double n1 = a.size(), n2 = b.size();
    double v1 = (s1.stddev * s1.stddev) / n1;
    double v2 = (s2.stddev * s2.stddev) / n2;
    if (v1 + v2 < 1e-12) return (std::abs(s1.mean - s2.mean) < 1e-6) ? 1.0 : 0.001;
    double t = std::abs(s1.mean - s2.mean) / std::sqrt(v1 + v2);
    double df = ((v1 + v2) * (v1 + v2)) / ((v1 * v1) / (n1 - 1) + (v2 * v2) / (n2 - 1));
    (void)df;
    // Normal approximation for p-value from t
    double p = 2.0 * (1.0 - 0.5 * (1.0 + std::erf(t / std::sqrt(2.0))));
    return std::max(0.0001, std::min(1.0, p));
}

RunMetrics executeSingleRun(const YCSBWorkloadConfig& wl_cfg,
                            Config engine_cfg,
                            const std::string& config_label,
                            int run_id)
{
    std::string db_dir = "./data_rigorous_" + config_label + "_r" + std::to_string(run_id);
    (void)system(("rm -rf " + db_dir + " && mkdir -p " + db_dir).c_str());
    engine_cfg.db_path = db_dir;

    LSMEngine engine(engine_cfg);

    // Load phase (single-threaded)
    auto load_ops = generateLoad(wl_cfg);
    for (auto& op : load_ops) {
        engine.insert(op.key, "v" + std::to_string(op.key));
    }
    engine.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // allow background compaction to settle

    // Transaction phase (strictly single-threaded I/O as mandated)
    auto ops = generateOps(wl_cfg);
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
                if (engine.search(op.key, val)) {
                    engine.insert(op.key, val + "_u");
                }
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
    return rm;
}

void runScaleBenchmark(int scale, int repeats) {
    std::string scale_str = (scale >= 1000000) ? (std::to_string(scale / 1000000) + "M")
                                               : (std::to_string(scale / 1000) + "K");
    std::cout << "\n============================================================\n";
    std::cout << ">>> Running Scale Point: " << scale_str << " (" << scale << " ops, "
              << repeats << (repeats == 1 ? " run)" : " repeats)") << "\n";
    std::cout << "============================================================\n";

    Config base_cfg;
    base_cfg.memtable_capacity    = 4096;
    base_cfg.max_levels           = 7;
    base_cfg.bloom_bits_per_key   = 10;
    base_cfg.bloom_max_bytes      = 256ULL * 1024 * 1024;
    base_cfg.block_cache_capacity = 64ULL * 1024 * 1024;

    Config cascade_cfg = base_cfg;
    cascade_cfg.memtable_type = MemtableType::CSB_PLUS;

    Config baseline_cfg = base_cfg;
    baseline_cfg.memtable_type        = MemtableType::SKIP_LIST;
    baseline_cfg.ahlc_write_rate_high = 1e18; // Force fixed leveling
    baseline_cfg.ahlc_skew_threshold  = 1.1;

    std::vector<std::function<YCSBWorkloadConfig(int)>> wl_funcs = {
        workloadA, workloadB, workloadC, workloadD, workloadE, workloadF
    };

    std::vector<RunMetrics> all_runs;

    for (int r = 1; r <= repeats; r++) {
        std::string raw_file = "bench/results/ycsb_" + scale_str + "_run" + std::to_string(r) + ".txt";
        std::ofstream raw_out(raw_file);
        raw_out << "# YCSB Benchmark Scale " << scale_str << " Run " << r << "\n";
        raw_out << "# Config\tWorkload\tTput(ops/s)\tWAF\tRAF\tSAF\tBloomFPR\tP50(us)\tP99(us)\tP99.9(us)\tAHLCSwitches\n";

        for (auto& fn : wl_funcs) {
            auto wl = fn(scale);
            std::cout << "  [" << scale_str << " Run " << r << "] " << wl.name << " Baseline..." << std::flush;
            auto rm_base = executeSingleRun(wl, baseline_cfg, "Baseline", r);
            std::cout << " " << std::fixed << std::setprecision(1) << rm_base.throughput_ops_sec / 1000.0 << " Kops/s"
                      << ", WAF=" << std::setprecision(2) << rm_base.waf << "\n";

            std::cout << "  [" << scale_str << " Run " << r << "] " << wl.name << " CASCADE..." << std::flush;
            auto rm_casc = executeSingleRun(wl, cascade_cfg, "CASCADE", r);
            std::cout << " " << std::fixed << std::setprecision(1) << rm_casc.throughput_ops_sec / 1000.0 << " Kops/s"
                      << ", WAF=" << std::setprecision(2) << rm_casc.waf << "\n";

            all_runs.push_back(rm_base);
            all_runs.push_back(rm_casc);

            raw_out << "Baseline\t" << wl.name << "\t" << rm_base.throughput_ops_sec << "\t"
                    << rm_base.waf << "\t" << rm_base.raf << "\t" << rm_base.saf << "\t"
                    << rm_base.bloom_fpr << "\t" << rm_base.p50_us << "\t" << rm_base.p99_us << "\t"
                    << rm_base.p999_us << "\t" << rm_base.ahlc_switches << "\n";

            raw_out << "CASCADE\t" << wl.name << "\t" << rm_casc.throughput_ops_sec << "\t"
                    << rm_casc.waf << "\t" << rm_casc.raf << "\t" << rm_casc.saf << "\t"
                    << rm_casc.bloom_fpr << "\t" << rm_casc.p50_us << "\t" << rm_casc.p99_us << "\t"
                    << rm_casc.p999_us << "\t" << rm_casc.ahlc_switches << "\n";
        }
        raw_out.close();
        std::cout << "  Saved raw output to: " << raw_file << "\n";
    }

    // Generate Summary
    std::string summary_file = "bench/results/ycsb_" + scale_str + "_summary.md";
    std::ofstream sum_out(summary_file);

    std::stringstream ss;
    ss << "### YCSB Benchmark Summary — Scale " << scale_str << " (" << scale << " ops"
       << (repeats == 1 ? ", single run)" : ", 3 repeats)") << "\n\n";

    if (repeats == 1) {
        ss << "| Workload | Engine | Tput (Kops/s) | WAF | RAF | SAF | Bloom FPR (%) | P50 (µs) | P99 (µs) | P99.9 (µs) | AHLC Switches |\n";
        ss << "|:---|:---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|\n";
        for (const auto& rm : all_runs) {
            ss << "| " << rm.workload_name << " | " << rm.config_name << " | "
               << std::fixed << std::setprecision(1) << rm.throughput_ops_sec / 1000.0 << " | "
               << std::setprecision(2) << rm.waf << " | " << rm.raf << " | " << rm.saf << " | "
               << std::setprecision(4) << rm.bloom_fpr * 100 << " | "
               << std::setprecision(1) << rm.p50_us << " | " << rm.p99_us << " | "
               << rm.p999_us << " | " << rm.ahlc_switches << " |\n";
        }
    } else {
        ss << "| Workload | Metric | Baseline (Mean ± Std) | CASCADE (Mean ± Std) | Diff (%) | Mann-Whitney U | p-value | Significance (p < 0.05) |\n";
        ss << "|:---|:---|:---:|:---:|:---:|:---:|:---:|:---:|\n";

        for (auto& fn : wl_funcs) {
            auto wl = fn(scale);
            std::vector<double> base_tput, casc_tput;
            std::vector<double> base_waf, casc_waf;
            std::vector<double> base_raf, casc_raf;
            std::vector<double> base_p99, casc_p99;

            for (const auto& r : all_runs) {
                if (r.workload_name == wl.name) {
                    if (r.config_name == "Baseline") {
                        base_tput.push_back(r.throughput_ops_sec / 1000.0);
                        base_waf.push_back(r.waf);
                        base_raf.push_back(r.raf);
                        base_p99.push_back(r.p99_us);
                    } else {
                        casc_tput.push_back(r.throughput_ops_sec / 1000.0);
                        casc_waf.push_back(r.waf);
                        casc_raf.push_back(r.raf);
                        casc_p99.push_back(r.p99_us);
                    }
                }
            }

            auto st_base_t = calcStats(base_tput);
            auto st_casc_t = calcStats(casc_tput);
            double diff_t = st_base_t.mean > 0 ? ((st_casc_t.mean - st_base_t.mean) / st_base_t.mean * 100.0) : 0;
            double u_stat = 0;
            double p_val = mannWhitneyU(base_tput, casc_tput, u_stat);
            double t_p_val = welchTTest(base_tput, casc_tput);

            ss << "| **" << wl.name << "** | Throughput (Kops/s) | "
               << std::fixed << std::setprecision(1) << st_base_t.mean << " ± " << st_base_t.stddev << " | "
               << st_casc_t.mean << " ± " << st_casc_t.stddev << " | "
               << (diff_t >= 0 ? "+" : "") << std::setprecision(1) << diff_t << "% | "
               << "U=" << (int)u_stat << " | p=" << std::setprecision(3) << p_val << " (t-test p=" << t_p_val << ") | "
               << (p_val < 0.05 ? "**Significant**" : "Not significant") << " |\n";

            auto st_base_w = calcStats(base_waf);
            auto st_casc_w = calcStats(casc_waf);
            double diff_w = st_base_w.mean > 0 ? ((st_casc_w.mean - st_base_w.mean) / st_base_w.mean * 100.0) : 0;
            ss << "| | WAF | "
               << std::fixed << std::setprecision(2) << st_base_w.mean << " ± " << st_base_w.stddev << " | "
               << st_casc_w.mean << " ± " << st_casc_w.stddev << " | "
               << (diff_w >= 0 ? "+" : "") << std::setprecision(1) << diff_w << "% | - | - | - |\n";

            auto st_base_r = calcStats(base_raf);
            auto st_casc_r = calcStats(casc_raf);
            double diff_r = st_base_r.mean > 0 ? ((st_casc_r.mean - st_base_r.mean) / st_base_r.mean * 100.0) : 0;
            ss << "| | RAF | "
               << std::fixed << std::setprecision(2) << st_base_r.mean << " ± " << st_base_r.stddev << " | "
               << st_casc_r.mean << " ± " << st_casc_r.stddev << " | "
               << (diff_r >= 0 ? "+" : "") << std::setprecision(1) << diff_r << "% | - | - | - |\n";

            auto st_base_p = calcStats(base_p99);
            auto st_casc_p = calcStats(casc_p99);
            double diff_p = st_base_p.mean > 0 ? ((st_casc_p.mean - st_base_p.mean) / st_base_p.mean * 100.0) : 0;
            ss << "| | P99 Latency (µs) | "
               << std::fixed << std::setprecision(1) << st_base_p.mean << " ± " << st_base_p.stddev << " | "
               << st_casc_p.mean << " ± " << st_casc_p.stddev << " | "
               << (diff_p >= 0 ? "+" : "") << std::setprecision(1) << diff_p << "% | - | - | - |\n";
        }
    }

    sum_out << ss.str();
    sum_out.close();

    std::cout << "\n" << ss.str() << "\n";
    std::cout << "Summary saved to: " << summary_file << "\n";
}

void profileAnomaly1() {
    std::cout << "\n============================================================\n";
    std::cout << ">>> Profiling Anomaly 1: CSB+ vs SkipList at 16 Threads\n";
    std::cout << "============================================================\n";

    const int THREADS = 16;
    const int OPS_PER_THREAD = 10000;
    const int TOTAL_OPS = THREADS * OPS_PER_THREAD;

    // Profile CSB+ Tree
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
    uint64_t csb_acq = ConcurrentCSBTree::total_lock_acquisitions_.load();

    // Profile SkipList
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
    uint64_t skl_acq = SkipListMemtable::total_lock_acquisitions_.load();

    std::ofstream out("bench/results/anomaly1_lock_profile.txt");
    std::stringstream ss;
    ss << "# Anomaly 1 Profiling: CSB+ rw_mu_ vs SkipList mu_ at 16 threads (N=" << TOTAL_OPS << " ops)\n";
    ss << "Data Structure\tThroughput(ops/s)\tTotal Lock Wait(ms)\tAcquisitions\tAvg Wait/Lock(us)\n";
    ss << "ConcurrentCSBTree\t" << std::fixed << std::setprecision(1) << TOTAL_OPS / csb_sec << "\t"
       << std::setprecision(2) << csb_wait_ns / 1e6 << "\t" << csb_acq << "\t"
       << (csb_acq > 0 ? (csb_wait_ns / 1000.0) / csb_acq : 0) << "\n";
    ss << "SkipListMemtable\t" << std::fixed << std::setprecision(1) << TOTAL_OPS / skl_sec << "\t"
       << std::setprecision(2) << skl_wait_ns / 1e6 << "\t" << skl_acq << "\t"
       << (skl_acq > 0 ? (skl_wait_ns / 1000.0) / skl_acq : 0) << "\n";

    out << ss.str();
    out.close();

    std::cout << ss.str() << "\n";
    std::cout << "Anomaly 1 profiling saved to bench/results/anomaly1_lock_profile.txt\n";
}

void profileAnomaly2() {
    std::cout << "\n============================================================\n";
    std::cout << ">>> Profiling Anomaly 2: Workload F (RMW) Strategy Switches & WAF\n";
    std::cout << "============================================================\n";

    int scale = 100000;
    auto wl = workloadF(scale);

    Config cfg;
    cfg.db_path                = "./data_anomaly2";
    cfg.memtable_type          = MemtableType::CSB_PLUS;
    cfg.memtable_capacity      = 2000;
    cfg.max_levels             = 5;
    cfg.ahlc_write_rate_high   = 10000.0; // sensitive to switches
    cfg.ahlc_hysteresis_epochs = 2;

    (void)system(("rm -rf " + cfg.db_path + " && mkdir -p " + cfg.db_path).c_str());

    LSMEngine engine(cfg);
    auto load_ops = generateLoad(wl);
    for (auto& op : load_ops) engine.insert(op.key, "v" + std::to_string(op.key));
    engine.flush();

    auto ops = generateOps(wl);
    Value val;
    for (const auto& op : ops) {
        if (op.type == OpType::READ) {
            engine.search(op.key, val);
        } else if (op.type == OpType::RMW) {
            if (engine.search(op.key, val)) engine.insert(op.key, val + "_u");
        }
    }
    engine.flush();

    auto switch_history = engine.ahlc().switchLog();

    std::ofstream out("bench/results/anomaly2_workloadF_profile.txt");
    std::stringstream ss;
    ss << "# Anomaly 2 Profiling: Workload F Strategy Switches\n";
    ss << "# Total Switches: " << switch_history.size() << "\n";
    ss << "# Final WAF: " << engine.metrics().waf() << "\n";
    ss << "Timestamp(ms)\tFromStrategy\tToStrategy\tWriteVelocity(B/s)\tSkewGini\n";

    auto stratToStr = [](Strategy s) {
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
           << entry.write_velocity << "\t"
           << entry.skew << "\n";
    }

    out << ss.str();
    out.close();

    (void)system(("rm -rf " + cfg.db_path).c_str());
    std::cout << ss.str() << "\n";
    std::cout << "Anomaly 2 profiling saved to bench/results/anomaly2_workloadF_profile.txt\n";
}

int main(int argc, char** argv) {
    std::string mode = "--all";
    int scale = 100000;
    int repeats = 3;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--scale" && i + 1 < argc) scale = std::stoi(argv[++i]);
        else if (arg == "--repeats" && i + 1 < argc) repeats = std::stoi(argv[++i]);
        else if (arg == "--anomaly1") mode = "--anomaly1";
        else if (arg == "--anomaly2") mode = "--anomaly2";
        else if (arg == "--all") mode = "--all";
        else mode = arg;
    }

    if (mode == "--anomaly1") {
        profileAnomaly1();
    } else if (mode == "--anomaly2") {
        profileAnomaly2();
    } else if (mode == "--single") {
        runScaleBenchmark(scale, repeats);
    } else if (mode == "--all") {
        // Step 4 & 5 full scale sequence:
        // 1. 100K ops (3 repeats)
        runScaleBenchmark(100000, 3);
        // 2. 500K ops (3 repeats)
        runScaleBenchmark(500000, 3);
        // 3. 1M ops (3 repeats)
        runScaleBenchmark(1000000, 3);
        // 4. 5M ops (1 repeat, labeled single run)
        runScaleBenchmark(5000000, 1);

        // Step 6 Profiling for the two anomalies
        profileAnomaly1();
        profileAnomaly2();
    }
    return 0;
}
