#!/usr/bin/env python3
"""
gen_workload_configs.py — Generate plain-text workload config files for all
11 workloads × 6 scales. Outputs to bench/configs/.

Each file documents the exact parameters used in the benchmark so a reviewer
can verify the workload setup without reading C++ source.
"""
import os

WORKLOADS = [
    # (name, read_frac, update_frac, insert_frac, delete_frac, scan_frac, rmw_frac, scan_length, zipfian_theta, description)
    ("A",   0.50, 0.50, 0.00, 0.00, 0.00, 0.00, 100,  0.99, "Standard YCSB-A: balanced 50/50 read/update"),
    ("B",   0.95, 0.05, 0.00, 0.00, 0.00, 0.00, 100,  0.99, "Standard YCSB-B: read-dominant 95/5"),
    ("C",   1.00, 0.00, 0.00, 0.00, 0.00, 0.00, 100,  0.99, "Standard YCSB-C: 100% read"),
    ("D",   0.95, 0.00, 0.05, 0.00, 0.00, 0.00, 100,  0.00, "Standard YCSB-D: read-latest (uniform insert)"),
    ("E",   0.00, 0.00, 0.05, 0.00, 0.95, 0.00, 100,  0.99, "Standard YCSB-E: 95% short scans / 5% insert"),
    ("F",   0.50, 0.00, 0.00, 0.00, 0.00, 0.50, 100,  0.99, "Standard YCSB-F: 50% read / 50% read-modify-write"),
    ("W",   0.01, 0.99, 0.00, 0.00, 0.00, 0.00, 100,  0.99, "Extension W: write-dominated ingest (APM/telemetry)"),
    ("RW",  0.50, 0.50, 0.00, 0.00, 0.00, 0.00, 100,  0.99, "Extension RW: balanced read/write (social/collaboration)"),
    ("RSW", 0.25, 0.25, 0.00, 0.00, 0.50, 0.00, 100,  0.99, "Extension RSW: scan-heavy reporting"),
    ("RS",  0.47, 0.47, 0.00, 0.00, 0.06, 0.00, 100,  0.99, "Extension RS: mixed scans analytics"),
    ("R",   0.95, 0.05, 0.00, 0.00, 0.00, 0.00, 100,  0.99, "Extension R: read-heavy caching"),
]

SCALES = [100_000, 500_000, 1_000_000, 5_000_000, 10_000_000, 15_000_000]
REPEATS = {100_000: 5, 500_000: 5, 1_000_000: 5, 5_000_000: 5, 10_000_000: 3, 15_000_000: 3}

os.makedirs("bench/configs", exist_ok=True)

for wl_name, rf, uf, ins_f, del_f, sf, rmw_f, sl, theta, desc in WORKLOADS:
    for scale in SCALES:
        key_space = max(1_000_000, scale * 2)
        record_count = scale // 2
        scale_str = f"{scale//1_000_000}M" if scale >= 1_000_000 else f"{scale//1_000}K"
        fname = f"bench/configs/workload_{wl_name}_{scale_str}.txt"

        is_extension = wl_name in ("W", "RW", "RSW", "RS", "R")
        ycsb_note = "Paper-defined extension (NOT standard YCSB)" if is_extension else "Standard YCSB workload"

        content = f"""# CASCADE Benchmark — Workload Configuration
# Workload: {wl_name} ({desc})
# Scale: {scale_str} ({scale:,} operations)
# YCSB status: {ycsb_note}
#
# These parameters are the exact values used in the benchmark harness.
# See include/workload.h workload{wl_name}() for the C++ definition.

workload_name    = {wl_name}
description      = {desc}
scale            = {scale}
num_ops          = {scale}
record_count     = {record_count}
key_space        = {key_space}
zipfian_theta    = {theta}

# Operation mix (must sum to 1.0)
read_frac        = {rf}
update_frac      = {uf}
insert_frac      = {ins_f}
delete_frac      = {del_f}
scan_frac        = {sf}
rmw_frac         = {rmw_f}
scan_length      = {sl}

# Benchmark protocol
repeats          = {REPEATS[scale]}
seed_base        = 42
seed_rule        = seed_base + run_id  (same per run_id across all 11 workloads)
load_seed        = 99                  (fixed across all runs)
direct_io_default = false

# Engine configuration (matches all rigorous_bench runs)
memtable_capacity     = 4096
max_levels            = 7
bloom_bits_per_key    = 14
bloom_max_bytes_MB    = 256
block_cache_MB        = 64
"""
        with open(fname, 'w') as f:
            f.write(content)

print(f"Generated {len(WORKLOADS) * len(SCALES)} config files in bench/configs/")
for wl_name, *_ in WORKLOADS:
    for scale in SCALES:
        scale_str = f"{scale//1_000_000}M" if scale >= 1_000_000 else f"{scale//1_000}K"
        print(f"  bench/configs/workload_{wl_name}_{scale_str}.txt")
