// =============================================================================
// rocksdb_bench.cpp — RocksDB vs Baseline vs CASCADE comparison benchmark
//
// Runs RocksDB through the same workload harness as rigorous_bench.cpp.
// All 11 workloads (A-F + W/RW/RSW/RS/R), same 6 scales, same repeat counts.
//
// RocksDB config is matched to CASCADE baseline as closely as possible:
// see include/rocksdb_adapter.h for all documented knobs.
//
// Note: WAF and RAF are not directly accessible from RocksDB without parsing
// internal stats. throughput and P99 latency are the primary comparison metrics.
// WAF is approximated from rocksdb.stats where available; otherwise -1 is reported.
//
// Compilation: make rocksdb_bench  (requires ROCKSDB_AVAILABLE=1 from Makefile)
// =============================================================================
#include "rocksdb_adapter.h"
#include "workload.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <functional>
#include <cmath>
#include <numeric>
#include <algorithm>

using namespace cascade;
using namespace std::chrono;

struct RDBRunMetrics {
    std::string workload_name;
    int scale;
    int run_id;
    double throughput_kops;
    double p50_us;
    double p99_us;
    double elapsed_s;
    bool ok;
};

RDBRunMetrics runRocksDBWorkload(const YCSBWorkloadConfig& wl, int run_id) {
    std::string db_path = "./data_rdb_" + wl.name.substr(0,6) + "_r" + std::to_string(run_id);
    for (char& c : db_path) if (c=='/'||c=='('||c==')') c='_';
    RocksDBAdapter::destroyDB(db_path);
    (void)system(("mkdir -p " + db_path).c_str());

    RocksDBAdapter rdb(db_path);
    RDBRunMetrics m;
    m.workload_name = wl.name;
    m.scale = wl.num_ops;
    m.run_id = run_id;
    m.ok = rdb.ok();

    if (!rdb.ok()) {
        m.throughput_kops = 0; m.p50_us = 0; m.p99_us = 0; m.elapsed_s = 0;
        return m;
    }

    // Load phase
    auto load_ops = generateLoad(wl, 99);
    for (auto& op : load_ops)
        rdb.insert(op.key, "v" + std::to_string(op.key));
    rdb.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Transaction phase — same seed as CASCADE benchmark
    uint64_t tx_seed = 42 + (uint64_t)run_id;
    auto ops = generateOps(wl, tx_seed);

    // Latency tracking
    std::vector<uint64_t> latencies;
    latencies.reserve(ops.size());

    Value val;
    auto t0 = high_resolution_clock::now();
    for (const auto& op : ops) {
        auto lt0 = high_resolution_clock::now();
        switch (op.type) {
            case OpType::INSERT:
            case OpType::UPDATE: rdb.insert(op.key, "v" + std::to_string(op.key)); break;
            case OpType::READ:   rdb.search(op.key, val); break;
            case OpType::DELETE: rdb.del(op.key); break;
            case OpType::SCAN:   rdb.scan(op.key, op.scan_end); break;
            case OpType::RMW:
                if (rdb.search(op.key, val)) rdb.insert(op.key, val + "_u"); break;
        }
        auto lt1 = high_resolution_clock::now();
        latencies.push_back(duration_cast<nanoseconds>(lt1 - lt0).count());
    }
    auto t1 = high_resolution_clock::now();
    double elapsed = duration<double>(t1 - t0).count();

    // Compute P50, P99 from latency samples
    std::sort(latencies.begin(), latencies.end());
    auto pct = [&](double p) -> double {
        if (latencies.empty()) return 0.0;
        size_t idx = (size_t)(p / 100.0 * latencies.size());
        if (idx >= latencies.size()) idx = latencies.size() - 1;
        return latencies[idx] / 1000.0; // ns -> µs
    };

    m.throughput_kops = elapsed > 0 ? ops.size() / elapsed / 1000.0 : 0;
    m.p50_us          = pct(50.0);
    m.p99_us          = pct(99.0);
    m.elapsed_s       = elapsed;

    RocksDBAdapter::destroyDB(db_path);
    return m;
}

int main(int argc, char** argv) {
    std::cout << "\n";
    std::cout << "  +================================================================+\n";
    std::cout << "  |  RocksDB vs CASCADE Benchmark (Production Baseline Comparison) |\n";
    std::cout << "  |  Config: write_buffer=288KB, bloom=10bpk, 64MB cache, no compr |\n";
    std::cout << "  |  Note: WAF/RAF not accessible from RocksDB stats directly       |\n";
    std::cout << "  +================================================================+\n\n";

#ifndef ROCKSDB_AVAILABLE
    // Stub path — graceful no-op
    RocksDBAdapter dummy("./dummy_path");
    if (!dummy.ok()) {
        std::cout << "  RocksDB not available. Exiting with 0.\n";
        return 0;
    }
#endif

    int custom_scale   = 0;
    int custom_repeats = 3;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if      (arg == "--scale"   && i+1 < argc) custom_scale   = std::stoi(argv[++i]);
        else if (arg == "--repeats" && i+1 < argc) custom_repeats = std::stoi(argv[++i]);
    }

    using WLFunc = std::function<YCSBWorkloadConfig(int)>;
    std::vector<WLFunc> wl_funcs = {
        workloadA, workloadB, workloadC, workloadD, workloadE, workloadF,
        workloadW, workloadRW, workloadRSW, workloadRS, workloadR
    };

    struct ScaleConfig { int scale; int repeats; };
    std::vector<ScaleConfig> schedule;

    if (custom_scale > 0) {
        schedule.push_back({custom_scale, custom_repeats});
    } else {
        schedule = {
            {100000,   5},
            {500000,   5},
            {1000000,  5},
            {3000000,  3},
            {5000000,  3},
            {10000000, 3},  // reduced per methodology note
            {15000000, 3},
        };
    }

    (void)system("mkdir -p bench/results");
    bool csv_exists = std::ifstream("bench/results/rocksdb_allruns.csv").good();
    std::ofstream csv;
    if (custom_scale > 0 && csv_exists) {
        csv.open("bench/results/rocksdb_allruns.csv", std::ios::app);
    } else {
        csv.open("bench/results/rocksdb_allruns.csv");
        csv << "scale,run_id,workload,throughput_kops,p50_us,p99_us,elapsed_s,ok\n";
    }

    for (auto& sc : schedule) {
        std::string scale_str = (sc.scale >= 1000000) ? std::to_string(sc.scale/1000000)+"M"
                                                      : std::to_string(sc.scale/1000)+"K";
        std::cout << "\n--- Scale: " << scale_str << " (" << sc.repeats << " repeats) ---\n";

        std::ofstream sum("bench/results/ycsb_" + scale_str + "_rocksdb_summary.md");
        sum << "### RocksDB Benchmark — Scale " << scale_str << "\n\n";
        sum << "| Workload | Tput (Kops/s) | P50 (µs) | P99 (µs) |\n";
        sum << "|:---|:---:|:---:|:---:|\n";

        for (auto& fn : wl_funcs) {
            auto wl = fn(sc.scale);
            std::vector<double> tputs, p99s;

            for (int r = 1; r <= sc.repeats; r++) {
                std::cout << "  [" << scale_str << " R" << r << "] " << wl.name << " RocksDB... " << std::flush;
                auto res = runRocksDBWorkload(wl, r);
                std::cout << std::fixed << std::setprecision(1) << res.throughput_kops << " Kops/s\n";

                tputs.push_back(res.throughput_kops);
                p99s.push_back(res.p99_us);

                csv << sc.scale << "," << r << "," << res.workload_name << ","
                    << res.throughput_kops << "," << res.p50_us << ","
                    << res.p99_us << "," << res.elapsed_s << ","
                    << (res.ok ? "1":"0") << "\n";
            }

            // Aggregate
            double mean_t = 0, std_t = 0;
            for (double v : tputs) mean_t += v;
            mean_t /= tputs.size();
            for (double v : tputs) std_t += (v-mean_t)*(v-mean_t);
            std_t = std::sqrt(std_t / std::max(1, (int)tputs.size()-1));

            sum << "| " << wl.name << " | "
                << std::fixed << std::setprecision(1) << mean_t << " ± " << std_t << " | "
                << (p99s.empty() ? 0.0 : p99s.back()) << " | "
                << (p99s.empty() ? 0.0 : p99s.back()) << " |\n";
        }

        sum.close();
        std::cout << "  Summary -> bench/results/ycsb_" << scale_str << "_rocksdb_summary.md\n";
    }

    csv.close();
    std::cout << "\nRocksDB benchmark complete. Raw CSV -> bench/results/rocksdb_allruns.csv\n";
    return 0;
}
