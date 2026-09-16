#pragma once
// =============================================================================
// metadata.h — Experiment Reproducibility Metadata
//
// Prints a machine-readable metadata block at benchmark startup capturing:
//   - Git commit, compiler, OS, hardware
//   - Full LSMEngine Config
//   - Workload parameters, seed, run_id
//
// Usage:
//   #include "metadata.h"
//   cascade::printExperimentMetadata(cfg, wl.name, wl.num_ops, run_id, seed);
//
// The output block is prefixed with "# META:" so it can be extracted from
// any benchmark log by grep without parsing the rest of the output.
//
// STEP 12 — Reproducibility Metadata
// =============================================================================
#include "common.h"
#include <iostream>
#include <string>
#include <cstdio>   // popen, fgets, FILE
#include <thread>   // hardware_concurrency
#include <cstring>  // memset

namespace cascade {

// ---------------------------------------------------------------------------
// Helper: run a shell command and return the first line of its stdout.
// Returns "(unavailable)" on failure — never throws.
// ---------------------------------------------------------------------------
inline std::string shellOneLine(const char* cmd) {
    char buf[512] = {};
#if !defined(_WIN32)
    FILE* f = popen(cmd, "r");
    if (!f) return "(unavailable)";
    if (!fgets(buf, sizeof(buf), f)) {
        pclose(f);
        return "(unavailable)";
    }
    pclose(f);
    // Strip trailing newline
    for (char& c : buf) if (c == '\n' || c == '\r') { c = '\0'; break; }
#endif
    std::string s(buf);
    return s.empty() ? "(unavailable)" : s;
}

// ---------------------------------------------------------------------------
// printExperimentMetadata — emit a reproducibility header to stdout.
//
// Parameters:
//   cfg       : LSMEngine Config used for this run
//   wl_name   : workload name string (e.g. "A(50R/50U)")
//   num_ops   : number of operations in the transaction phase
//   run_id    : repeat ID (1-based)
//   seed      : seed passed to generateOps() for the transaction phase
//   extra_tags: optional extra key=value pairs (comma-separated)
// ---------------------------------------------------------------------------
inline void printExperimentMetadata(
    const Config&      cfg,
    const std::string& wl_name,
    int                num_ops,
    int                run_id,
    uint64_t           seed,
    const std::string& extra_tags = "")
{
    // Git info
    std::string git_commit = shellOneLine("git rev-parse HEAD 2>/dev/null");
    std::string git_dirty  = shellOneLine("git status --porcelain 2>/dev/null | head -1");
    // OS info
    std::string uname_info = shellOneLine("uname -srm 2>/dev/null");
    // Compiler info (compile-time macro)
    std::string compiler_ver = __VERSION__;
    // CPU count
    unsigned cpu_count = std::thread::hardware_concurrency();

    std::cout << "\n";
    std::cout << "# META: ============================================================\n";
    std::cout << "# META: CASCADE Experiment Metadata\n";
    std::cout << "# META: ============================================================\n";

    // Build/environment
    std::cout << "# META: git_commit          = " << git_commit << "\n";
    if (!git_dirty.empty())
        std::cout << "# META: git_dirty           = " << git_dirty << " (WARN: uncommitted changes)\n";
    std::cout << "# META: compiler            = " << compiler_ver << "\n";
    std::cout << "# META: os                  = " << uname_info << "\n";
    std::cout << "# META: cpu_count           = " << cpu_count << "\n";

    // Workload
    std::cout << "# META: workload            = " << wl_name << "\n";
    std::cout << "# META: num_ops             = " << num_ops << "\n";
    std::cout << "# META: run_id              = " << run_id << "\n";
    std::cout << "# META: seed                = " << seed << "\n";

    // Engine config
    std::cout << "# META: memtable_type       = "
              << (cfg.memtable_type == MemtableType::CSB_PLUS ? "CSB_PLUS" : "SKIP_LIST") << "\n";
    std::cout << "# META: memtable_capacity   = " << cfg.memtable_capacity << " keys\n";
    std::cout << "# META: max_levels          = " << cfg.max_levels << "\n";
    std::cout << "# META: bloom_bits_per_key  = " << cfg.bloom_bits_per_key << "\n";
    std::cout << "# META: bloom_max_bytes     = " << cfg.bloom_max_bytes << "\n";
    std::cout << "# META: block_cache_cap     = " << cfg.block_cache_capacity << "\n";
    std::cout << "# META: direct_io           = " << (cfg.direct_io ? "true" : "false") << "\n";

    // AHLC config
    // tau_v = ahlc_write_rate_high (bytes/sec); see common.h for units note
    std::cout << "# META: tau_v_bps           = " << cfg.ahlc_write_rate_high
              << " (bytes/sec; see common.h for operational note)\n";
    std::cout << "# META: ahlc_skew_threshold = " << cfg.ahlc_skew_threshold << "\n";
    std::cout << "# META: ahlc_hysteresis_epochs = " << cfg.ahlc_hysteresis_epochs << "\n";
    std::cout << "# META: ahlc_ewma_alpha     = " << cfg.ahlc_ewma_alpha << "\n";

    // Optional extras (e.g. config label, system ID)
    if (!extra_tags.empty())
        std::cout << "# META: extra               = " << extra_tags << "\n";

    std::cout << "# META: ============================================================\n\n";
}

} // namespace cascade
