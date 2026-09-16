#pragma once
// =============================================================================
// metrics.h — RUM Triad + throughput + latency percentile tracking
// WAF = bytes_written_to_sstables / bytes_logically_ingested
// RAF = block_reads_for_queries  / total_reads
// SAF = physical_keys_on_disk    / logical_live_keys
// =============================================================================
#include <atomic>
#include <vector>
#include <algorithm>
#include <numeric>
#include <mutex>
#include <cstdint>
#include <cmath>
#include <string>
#include <sstream>
#include <iomanip>
#include <iostream>

namespace cascade {

// ---------------------------------------------------------------------------
// HDR-style latency histogram (nanoseconds, 2^20 buckets)
// ---------------------------------------------------------------------------
class LatencyHistogram {
public:
    static constexpr int BUCKETS = 256;
    std::atomic<uint64_t> buckets_[BUCKETS]{};
    std::atomic<uint64_t> total_ns_{0};
    std::atomic<uint64_t> count_{0};

    LatencyHistogram() {
        for (int i = 0; i < BUCKETS; i++) buckets_[i].store(0, std::memory_order_relaxed);
    }

    void record(uint64_t ns) {
        count_.fetch_add(1, std::memory_order_relaxed);
        total_ns_.fetch_add(ns, std::memory_order_relaxed);
        int b = 0;
        uint64_t v = ns / 100;
        while (v > 0 && b < BUCKETS - 1) {
            v >>= 1;
            b++;
        }
        buckets_[b].fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t percentile(double p) const {
        uint64_t total = count_.load(std::memory_order_relaxed);
        if (total == 0) return 0;
        uint64_t target = (uint64_t)(p / 100.0 * total);
        uint64_t acc = 0;
        for (int b = 0; b < BUCKETS; b++) {
            acc += buckets_[b].load(std::memory_order_relaxed);
            if (acc >= target) {
                if (b == 0) return 100;
                return (1ULL << (b - 1)) * 100;
            }
        }
        return 10000000;
    }

    double mean() {
        uint64_t c = count_.load(std::memory_order_relaxed);
        return c ? (double)total_ns_.load(std::memory_order_relaxed) / c : 0.0;
    }

    void reset() {
        for (int i = 0; i < BUCKETS; i++) buckets_[i].store(0, std::memory_order_relaxed);
        total_ns_.store(0, std::memory_order_relaxed);
        count_.store(0, std::memory_order_relaxed);
    }

    uint64_t count() const { return count_.load(std::memory_order_relaxed); }
};

// ---------------------------------------------------------------------------
// Engine-wide metric counters
// ---------------------------------------------------------------------------
struct EngineMetrics {
    // Writes
    std::atomic<int64_t>  total_inserts{0};
    std::atomic<int64_t>  total_deletes{0};
    std::atomic<int64_t>  total_flushes{0};
    std::atomic<int64_t>  total_compactions{0};
    std::atomic<int64_t>  strategy_switches{0};

    // Reads
    std::atomic<int64_t>  total_reads{0};
    std::atomic<int64_t>  read_hits{0};
    std::atomic<int64_t>  bloom_probes{0};
    std::atomic<int64_t>  bloom_false_positives{0};
    std::atomic<int64_t>  sstable_block_reads{0};

    // I/O byte accounting (for WAF / RAF / SAF)
    std::atomic<int64_t>  bytes_ingested_logical{0};  // user write bytes (logical)
    std::atomic<int64_t>  bytes_written_wal{0};        // WAL bytes
    // SSTable write bytes split for traceability:
    //   bytes_written_flush:      MemTable → L0 flush I/O
    //   bytes_written_compaction: L_i → L_{i+1} compaction I/O
    //   bytes_written_sstables:   combined (kept for backward compat)
    std::atomic<int64_t>  bytes_written_flush{0};
    std::atomic<int64_t>  bytes_written_compaction{0};
    std::atomic<int64_t>  bytes_written_sstables{0};  // = flush + compaction
    std::atomic<int64_t>  bytes_read_sstables{0};     // SSTable reads
    std::atomic<int64_t>  logical_live_keys{0};
    std::atomic<int64_t>  physical_keys_on_disk{0};

    // Latency histograms
    LatencyHistogram write_latency;
    LatencyHistogram read_latency;
    LatencyHistogram scan_latency;

    // Derived RUM metrics
    // WAF = (wal_bytes + sstable_bytes) / logical_bytes.
    // WAL bytes and SSTable bytes are tracked separately (not double-counted).
    // wal bytes = bytes_written_wal  (WAL append I/O)
    // sstable bytes = bytes_written_sstables  (flush + compaction I/O combined)
    double waf() const {
        int64_t in = bytes_ingested_logical.load();
        if (in == 0) return 1.0;
        return (double)(bytes_written_wal.load() + bytes_written_sstables.load()) / in;
    }
    double raf() const {
        int64_t r = total_reads.load();
        if (r == 0) return 0.0;
        return (double)sstable_block_reads.load() / r;
    }
    double saf() const {
        int64_t live = logical_live_keys.load();
        if (live == 0) return 1.0;
        return (double)physical_keys_on_disk.load() / live;
    }
    double bloom_fpr() const {
        int64_t p = bloom_probes.load();
        if (p == 0) return 0.0;
        return (double)bloom_false_positives.load() / p;
    }

    // Print individual WAF accounting components for auditability.
    // This does NOT change the WAF formula — it only exposes the inputs.
    // Useful for verifying:
    //   (1) WAL bytes are NOT double-counted in the SSTable total.
    //   (2) The split between flush I/O and compaction I/O.
    void printWAFComponents(const std::string& label = "") const {
        int64_t logical   = bytes_ingested_logical.load();
        int64_t wal       = bytes_written_wal.load();
        int64_t flush_io  = bytes_written_flush.load();
        int64_t compact_io = bytes_written_compaction.load();
        int64_t sst_total = bytes_written_sstables.load();
        double  waf_val   = (logical > 0)
            ? (double)(wal + sst_total) / logical : 1.0;
        std::cout << "\n  WAF Accounting" << (label.empty() ? "" : " [" + label + "]")
                  << ":\n"
                  << "    logical_bytes    = " << logical    << "\n"
                  << "    wal_bytes        = " << wal        << "\n"
                  << "    flush_bytes      = " << flush_io   << "\n"
                  << "    compaction_bytes = " << compact_io << "\n"
                  << "    sstable_total    = " << sst_total  << " (flush+compaction)\n"
                  << "    WAF              = " << std::fixed << std::setprecision(3)
                  << waf_val << "x\n";
    }

    void print(const std::string& label, double elapsed_sec) const {
        double throughput_write = elapsed_sec > 0 ? total_inserts.load() / elapsed_sec : 0;
        double throughput_read  = elapsed_sec > 0 ? total_reads.load()   / elapsed_sec : 0;
        std::cout << "\n  +----------------------------------------------------------+\n";
        std::cout << "  |  " << std::left << std::setw(56) << label << "|\n";
        std::cout << "  +----------------------------------------------------------+\n";
        std::cout << "  |  Inserts:         " << std::setw(12) << total_inserts.load()    << "                         |\n";
        std::cout << "  |  Reads:           " << std::setw(12) << total_reads.load()      << "                         |\n";
        std::cout << "  |  Read Hits:       " << std::setw(12) << read_hits.load()        << "                         |\n";
        std::cout << "  |  Flushes:         " << std::setw(12) << total_flushes.load()    << "                         |\n";
        std::cout << "  |  Compactions:     " << std::setw(12) << total_compactions.load()<< "                         |\n";
        std::cout << "  |  AHLC Switches:   " << std::setw(12) << strategy_switches.load()<< "                         |\n";
        std::cout << "  |  Bloom FPR:       " << std::setw(10) << std::fixed << std::setprecision(4) << bloom_fpr() * 100 << " %                       |\n";
        std::cout << "  |  WAF:             " << std::setw(10) << std::fixed << std::setprecision(3) << waf() << "x                        |\n";
        std::cout << "  |  RAF:             " << std::setw(10) << std::fixed << std::setprecision(3) << raf() << " blk/query               |\n";
        std::cout << "  |  SAF:             " << std::setw(10) << std::fixed << std::setprecision(3) << saf() << "x                        |\n";
        std::cout << "  |  Write throughput:" << std::setw(12) << (int64_t)throughput_write << " ops/sec              |\n";
        std::cout << "  |  Read throughput: " << std::setw(12) << (int64_t)throughput_read  << " ops/sec              |\n";
        std::cout << "  +----------------------------------------------------------+\n";
    }
};

} // namespace cascade
