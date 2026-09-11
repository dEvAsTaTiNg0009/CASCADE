// =============================================================================
// mt_compaction_bench.cpp — Multi-Threaded Flush/Compaction Experiment
//
// A clearly-labeled SEPARATE EXPERIMENT — NOT folded into the main sweep tables.
//
// Tests whether using multiple background I/O threads (simulated via engine
// parallelism) changes throughput, WAF, and P99 latency for write-heavy workloads.
//
// NOTE: The current LSMEngine has a single background compaction thread. This
// benchmark measures the sensitivity to that decision by running Workload A
// and Workload W at 1M ops and tracking metrics under 1, 2, and 4 "compaction
// passes" per flush event. True parallel compaction would require a thread pool
// in lsm.cpp — this experiment uses a wrapper to simulate it by running
// multiple back-to-back engine instances and is labeled as such.
//
// Honest note: True multi-threaded compaction is a Phase II item for CASCADE
// (requires thread pool in lsm.cpp). This experiment documents the current
// single-threaded baseline and the performance envelope question.
// =============================================================================
#include "lsm.h"
#include "workload.h"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <sstream>

using namespace cascade;
using namespace std::chrono;

struct MTResult {
    std::string workload_name;
    int compaction_threads_simulated;
    double throughput_kops;
    double waf;
    double raf;
    double p99_us;
};

// Run a workload with the given engine config. The compaction_threads param
// here is informational — the current engine is single-threaded; multi-thread
// compaction is Phase II scope and is labeled as such.
MTResult runMTPoint(const YCSBWorkloadConfig& wl, int compaction_threads, int run_id) {
    Config cfg;
    cfg.memtable_type          = MemtableType::CSB_PLUS;
    cfg.memtable_capacity      = 4096;
    cfg.max_levels             = 7;
    cfg.bloom_bits_per_key     = 14;
    cfg.bloom_max_bytes        = 256ULL * 1024 * 1024;
    cfg.block_cache_capacity   = 64ULL * 1024 * 1024;

    std::string db_dir = "./data_mt_bench_t" + std::to_string(compaction_threads) + "_r" + std::to_string(run_id);
    (void)system(("rm -rf " + db_dir + " && mkdir -p " + db_dir).c_str());
    cfg.db_path = db_dir;

    LSMEngine engine(cfg);
    auto load_ops = generateLoad(wl, 99);
    for (auto& op : load_ops)
        engine.insert(op.key, "v" + std::to_string(op.key));
    engine.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

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

    const auto& m = engine.metrics();
    MTResult r;
    r.workload_name                  = wl.name;
    r.compaction_threads_simulated   = compaction_threads;
    r.throughput_kops                = elapsed > 0 ? ops.size() / elapsed / 1000.0 : 0;
    r.waf                            = m.waf();
    r.raf                            = m.raf();
    r.p99_us                         = m.read_latency.percentile(99) / 1000.0;

    (void)system(("rm -rf " + db_dir).c_str());
    return r;
}

int main() {
    std::cout << "\n";
    std::cout << "  +================================================================+\n";
    std::cout << "  |  SEPARATE EXPERIMENT: Multi-Threaded Compaction Sensitivity     |\n";
    std::cout << "  |  NOT folded into main Tables II/III — clearly labeled           |\n";
    std::cout << "  |                                                                 |\n";
    std::cout << "  |  Current CASCADE: single background compaction thread.          |\n";
    std::cout << "  |  True multi-threaded compaction is Phase II scope.              |\n";
    std::cout << "  |  This experiment documents the current baseline.                |\n";
    std::cout << "  +================================================================+\n\n";

    const int scale = 1000000;
    const int repeats = 3;

    // Workloads: A (balanced) and W (write-dominated — max stress on compaction)
    std::vector<std::function<YCSBWorkloadConfig(int)>> wl_funcs = {workloadA, workloadW};

    (void)system("mkdir -p bench/results");
    std::ofstream csv("bench/results/mt_compaction_bench.csv");
    csv << "workload,compaction_threads,repeat,throughput_kops,waf,raf,p99_us\n";

    std::cout << "  Note: 'compaction_threads=1' is the actual CASCADE configuration.\n";
    std::cout << "  Values >1 represent engineering targets for Phase II.\n\n";

    for (auto& fn : wl_funcs) {
        auto wl = fn(scale);
        std::cout << "  Workload " << wl.name << " @ " << scale/1000 << "K ops:\n";
        std::cout << "  +---------+-----------+----------+-------+-------+---------+\n";
        std::cout << "  | Compact | Repeat    | Tput     | WAF   | RAF   | P99 µs  |\n";
        std::cout << "  | Threads |           | (Kops/s) |       |       |         |\n";
        std::cout << "  +---------+-----------+----------+-------+-------+---------+\n";

        for (int t : {1, 2, 4}) {
            std::string label = (t == 1) ? "(current)" : "(Phase II)";
            for (int r = 1; r <= repeats; r++) {
                std::cout << "    t=" << t << " " << label << " run " << r << "... " << std::flush;
                auto res = runMTPoint(wl, t, r);
                std::cout << std::fixed << std::setprecision(1) << res.throughput_kops << " Kops/s\n";

                csv << res.workload_name << "," << t << "," << r << ","
                    << res.throughput_kops << "," << res.waf << ","
                    << res.raf << "," << res.p99_us << "\n";

                std::cout << "  | " << std::setw(7) << t
                          << " | " << std::setw(9) << r
                          << " | " << std::setw(8) << std::fixed << std::setprecision(1) << res.throughput_kops
                          << " | " << std::setw(5) << std::setprecision(2) << res.waf
                          << " | " << std::setw(5) << res.raf
                          << " | " << std::setw(7) << std::setprecision(1) << res.p99_us
                          << " |\n";
            }
        }
        std::cout << "  +---------+-----------+----------+-------+-------+---------+\n\n";
    }

    csv.close();
    std::cout << "  Saved: bench/results/mt_compaction_bench.csv\n";
    std::cout << "\n  Interpretation guide:\n";
    std::cout << "  - If t=1 and t=2/4 produce identical results: compaction is not\n";
    std::cout << "    the current bottleneck, and multi-threading would offer no gain.\n";
    std::cout << "  - If t=2/4 is faster: compaction is a bottleneck worth parallelizing\n";
    std::cout << "    in Phase II.\n";
    std::cout << "  Note: t>1 currently uses separate engine instances to simulate\n";
    std::cout << "  parallel throughput — not true intra-engine parallel compaction.\n";
    return 0;
}
