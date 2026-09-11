// =============================================================================
// ahlc_sweep.cpp — AHLC Hysteresis Sensitivity Sweep
//
// Sweeps:
//   ahlc_hysteresis_epochs: {0, 1, 2, 3, 5, 8}
//   ahlc_write_rate_high (τ_v): {5000, 10000, 20000, 50000} bytes/sec
//
// Workloads:
//   F (500K and 1M) — the thrashing case: reports switch count and throughput
//   A and B (1M only) — control case: measures whether wider hysteresis
//   hurts throughput on workloads that legitimately need fast switching
//
// Answers two questions (reported honestly, even if the fix doesn't fully work):
//   (1) Does widening hysteresis suppress the 6-switches-in-65ms thrashing?
//   (2) Does it cost throughput on A/B that benefit from fast switching?
//
// Output: bench/results/ahlc_sweep_<scale>.csv + console markdown table
// =============================================================================
#include "lsm.h"
#include "workload.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <sstream>

using namespace cascade;
using namespace std::chrono;

struct SweepResult {
    int    scale;
    int    hysteresis_epochs;
    double tau_v;
    std::string workload_name;
    int    ahlc_switches;
    double throughput_kops;
    double waf;
};

SweepResult runSweepPoint(const YCSBWorkloadConfig& wl,
                           int hysteresis_epochs,
                           double tau_v,
                           int run_id)
{
    Config cfg;
    cfg.memtable_type          = MemtableType::CSB_PLUS;
    cfg.memtable_capacity      = 4096;
    cfg.max_levels             = 7;
    cfg.bloom_bits_per_key     = 14;
    cfg.bloom_max_bytes        = 256ULL * 1024 * 1024;
    cfg.block_cache_capacity   = 64ULL * 1024 * 1024;
    cfg.ahlc_hysteresis_epochs = hysteresis_epochs;
    cfg.ahlc_write_rate_high   = tau_v;

    std::string db_dir = "./data_ahlc_sweep_h" + std::to_string(hysteresis_epochs)
                       + "_t" + std::to_string((int)tau_v) + "_r" + std::to_string(run_id);
    (void)system(("rm -rf " + db_dir + " && mkdir -p " + db_dir).c_str());
    cfg.db_path = db_dir;

    LSMEngine engine(cfg);

    auto load_ops = generateLoad(wl, 99);
    for (auto& op : load_ops)
        engine.insert(op.key, "v" + std::to_string(op.key));
    engine.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto ops = generateOps(wl, 42 + (uint64_t)run_id);
    Value val;
    auto t0 = high_resolution_clock::now();
    for (const auto& op : ops) {
        switch (op.type) {
            case OpType::INSERT:
            case OpType::UPDATE:
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

    SweepResult r;
    r.scale              = wl.num_ops;
    r.hysteresis_epochs  = hysteresis_epochs;
    r.tau_v              = tau_v;
    r.workload_name      = wl.name;
    r.ahlc_switches      = engine.ahlc().switches();
    r.throughput_kops    = elapsed > 0 ? ops.size() / elapsed / 1000.0 : 0;
    r.waf                = engine.metrics().waf();

    (void)system(("rm -rf " + db_dir).c_str());
    return r;
}

int main() {
    std::cout << "\n";
    std::cout << "  +============================================================+\n";
    std::cout << "  |  AHLC Hysteresis Sensitivity Sweep                         |\n";
    std::cout << "  |  Axes: hysteresis_epochs × tau_v × workload × scale        |\n";
    std::cout << "  +============================================================+\n\n";

    // Sweep axes
    std::vector<int>    hysteresis_vals = {0, 1, 2, 3, 5, 8};
    std::vector<double> tau_v_vals      = {5000.0, 10000.0, 20000.0, 50000.0};
    std::vector<int>    scales_F        = {500000, 1000000}; // thrashing case
    int                 scale_ctrl      = 1000000;           // control case

    (void)system("mkdir -p bench/results");

    // -----------------------------------------------------------------------
    // Primary: Workload F sweep at 500K and 1M
    // -----------------------------------------------------------------------
    for (int sc : scales_F) {
        std::string scale_str = (sc >= 1000000) ? std::to_string(sc/1000000) + "M"
                                                : std::to_string(sc/1000) + "K";
        std::string csv_path  = "bench/results/ahlc_sweep_" + scale_str + ".csv";
        std::ofstream csv(csv_path);
        csv << "scale,workload,hysteresis_epochs,tau_v,ahlc_switches,throughput_kops,waf\n";

        std::cout << "\n--- Workload F Sweep @ " << scale_str << " ---\n";
        std::cout << std::left
                  << std::setw(6)  << "Hyst"
                  << std::setw(10) << "τ_v"
                  << std::setw(10) << "Switches"
                  << std::setw(14) << "Tput(Kops/s)"
                  << std::setw(8)  << "WAF"
                  << "\n";
        std::cout << std::string(48, '-') << "\n";

        std::vector<SweepResult> results_F;
        auto wl_F = workloadF(sc);

        for (int h : hysteresis_vals) {
            for (double tv : tau_v_vals) {
                std::cout << "  h=" << h << " τ=" << (int)tv << "... " << std::flush;
                auto r = runSweepPoint(wl_F, h, tv, h*10 + (int)(tv/1000));
                results_F.push_back(r);
                std::cout << "switches=" << r.ahlc_switches << " tput=" << std::fixed
                          << std::setprecision(1) << r.throughput_kops << " Kops/s WAF=" << r.waf << "\n";
                csv << sc << "," << r.workload_name << "," << h << "," << tv << ","
                    << r.ahlc_switches << "," << r.throughput_kops << "," << r.waf << "\n";
            }
        }
        csv.close();

        // Print markdown table
        std::cout << "\n#### Workload F Hysteresis Sweep — Scale " << scale_str << "\n\n";
        std::cout << "| Hyst epochs | τ_v (B/s) | Switches | Tput (Kops/s) | WAF |\n";
        std::cout << "|:---:|:---:|:---:|:---:|:---:|\n";
        for (auto& r : results_F) {
            std::cout << "| " << r.hysteresis_epochs
                      << " | " << (int)r.tau_v
                      << " | " << r.ahlc_switches
                      << " | " << std::fixed << std::setprecision(1) << r.throughput_kops
                      << " | " << std::setprecision(2) << r.waf
                      << " |\n";
        }
        std::cout << "\nSaved: " << csv_path << "\n";
    }

    // -----------------------------------------------------------------------
    // Control: Workloads A and B at 1M to detect sluggishness from over-hysteresis
    // -----------------------------------------------------------------------
    {
        std::string scale_str = "1M";
        std::string csv_path  = "bench/results/ahlc_sweep_ctrl_" + scale_str + ".csv";
        std::ofstream csv(csv_path);
        csv << "scale,workload,hysteresis_epochs,tau_v,ahlc_switches,throughput_kops,waf\n";

        std::cout << "\n--- Control (A & B) Sweep @ " << scale_str << " (checks for sluggishness) ---\n";

        for (auto& fn : {workloadA, workloadB}) {
            auto wl = fn(scale_ctrl);
            std::cout << "\n  Workload " << wl.name << ":\n";
            std::cout << std::left
                      << std::setw(6)  << "Hyst"
                      << std::setw(10) << "τ_v"
                      << std::setw(10) << "Switches"
                      << std::setw(14) << "Tput(Kops/s)"
                      << std::setw(8)  << "WAF"
                      << "\n";
            std::cout << std::string(48, '-') << "\n";

            for (int h : hysteresis_vals) {
                for (double tv : tau_v_vals) {
                    std::cout << "  h=" << h << " τ=" << (int)tv << "... " << std::flush;
                    auto r = runSweepPoint(wl, h, tv, h*10 + (int)(tv/1000));
                    std::cout << "switches=" << r.ahlc_switches << " tput=" << std::fixed
                              << std::setprecision(1) << r.throughput_kops << " Kops/s WAF=" << r.waf << "\n";
                    csv << scale_ctrl << "," << r.workload_name << "," << h << "," << tv << ","
                        << r.ahlc_switches << "," << r.throughput_kops << "," << r.waf << "\n";
                }
            }
        }
        csv.close();
        std::cout << "\nSaved: " << csv_path << "\n";
    }

    // -----------------------------------------------------------------------
    // Summary: key question answers
    // -----------------------------------------------------------------------
    std::cout << "\n================================================================\n";
    std::cout << "AHLC Hysteresis Sweep Complete.\n";
    std::cout << "Key questions answered by the sweep data:\n";
    std::cout << "  (1) Workload F: Does hysteresis reduce switch count?\n";
    std::cout << "      See bench/results/ahlc_sweep_500K.csv and ahlc_sweep_1M.csv\n";
    std::cout << "  (2) Workloads A/B: Does over-hysteresis reduce throughput?\n";
    std::cout << "      See bench/results/ahlc_sweep_ctrl_1M.csv\n";
    std::cout << "  Report BOTH directions honestly regardless of outcome.\n";
    std::cout << "================================================================\n";

    return 0;
}
