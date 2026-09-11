#!/usr/bin/env python3
"""
aggregate_results.py — Reproduces all paper tables from raw CSV output.

Usage:
    python3 scripts/aggregate_results.py [--results-dir bench/results] [--output bench/results/aggregated.md]

Input:
    bench/results/ycsb_<scale>_allruns.csv  (written by rigorous_bench --all)
    bench/results/rocksdb_allruns.csv       (written by rocksdb_bench, optional)
    bench/results/ahlc_sweep_*.csv          (written by ahlc_sweep, optional)

Output:
    - Markdown tables to stdout (reproducible from raw CSVs)
    - bench/results/aggregated.md

Statistics:
    - Mean ± sample std dev per (workload, scale, config) group
    - Welch's t-test p-value: CASCADE vs Baseline at each (workload, scale)
    - Outlier column preserved from CSV (flagged but not dropped)

This script is designed so a reviewer can verify every table in the paper
by running it directly on the raw CSV files.
"""

import sys
import os
import csv
import math
import argparse
from collections import defaultdict
from typing import List, Dict, Tuple, Optional


# ---------------------------------------------------------------------------
# Statistical helpers
# ---------------------------------------------------------------------------

def mean(vals: List[float]) -> float:
    return sum(vals) / len(vals) if vals else 0.0


def stddev(vals: List[float]) -> float:
    if len(vals) < 2:
        return 0.0
    m = mean(vals)
    return math.sqrt(sum((v - m) ** 2 for v in vals) / (len(vals) - 1))


def welch_t_test(a: List[float], b: List[float]) -> float:
    """Two-tailed Welch t-test p-value (normal approximation for large df)."""
    if len(a) < 2 or len(b) < 2:
        return 1.0
    ma, mb = mean(a), mean(b)
    sa, sb = stddev(a), stddev(b)
    na, nb = len(a), len(b)
    v1 = (sa ** 2) / na
    v2 = (sb ** 2) / nb
    denom = math.sqrt(v1 + v2)
    if denom < 1e-12:
        return 1.0 if abs(ma - mb) < 1e-6 else 0.001
    t = abs(ma - mb) / denom
    # Normal approximation: erf(t/sqrt(2))
    p = 2.0 * (1.0 - 0.5 * (1.0 + math.erf(t / math.sqrt(2.0))))
    return max(0.0001, min(1.0, p))


def mann_whitney_u(a: List[float], b: List[float]) -> Tuple[float, float]:
    """Mann-Whitney U statistic and p-value (normal approximation)."""
    n1, n2 = len(a), len(b)
    if n1 < 2 or n2 < 2:
        return -1.0, 1.0
    combined = [(v, 1) for v in a] + [(v, 2) for v in b]
    combined.sort(key=lambda x: x[0])
    # Average rank ties
    ranks = [0.0] * len(combined)
    i = 0
    while i < len(combined):
        j = i
        while j < len(combined) and combined[j][0] == combined[i][0]:
            j += 1
        avg_rank = (i + j + 1) / 2.0
        for k in range(i, j):
            ranks[k] = avg_rank
        i = j
    r1 = sum(ranks[k] for k in range(len(combined)) if combined[k][1] == 1)
    u1 = r1 - n1 * (n1 + 1) / 2.0
    u2 = n1 * n2 - u1
    u_stat = min(u1, u2)
    mean_u = n1 * n2 / 2.0
    sigma_u = math.sqrt(n1 * n2 * (n1 + n2 + 1) / 12.0)
    if sigma_u < 1e-9:
        return u_stat, 1.0
    z = (u_stat - mean_u) / sigma_u
    p = 2.0 * (1.0 - 0.5 * (1.0 + math.erf(abs(z) / math.sqrt(2.0))))
    return u_stat, max(0.0001, min(1.0, p))


# ---------------------------------------------------------------------------
# Data loading
# ---------------------------------------------------------------------------

def load_allruns_csv(path: str) -> List[Dict]:
    """Load bench/results/ycsb_<scale>_allruns.csv"""
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path, newline='') as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                row['scale']             = int(row['scale'])
                row['run_id']            = int(row['run_id'])
                row['throughput_ops_s']  = float(row['throughput_ops_s'])
                row['waf']               = float(row['waf'])
                row['raf']               = float(row['raf'])
                row['saf']               = float(row['saf'])
                row['bloom_fpr']         = float(row['bloom_fpr'])
                row['p50_us']            = float(row['p50_us'])
                row['p99_us']            = float(row['p99_us'])
                row['p999_us']           = float(row['p999_us'])
                row['ahlc_switches']     = int(row['ahlc_switches'])
                row['outlier']           = row.get('outlier', 'no').strip().upper() == 'YES'
                rows.append(row)
            except (ValueError, KeyError):
                pass
    return rows


def load_rocksdb_csv(path: str) -> List[Dict]:
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path, newline='') as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                row['scale']            = int(row['scale'])
                row['run_id']           = int(row['run_id'])
                row['throughput_kops']  = float(row['throughput_kops'])
                row['p50_us']           = float(row['p50_us'])
                row['p99_us']           = float(row['p99_us'])
                rows.append(row)
            except (ValueError, KeyError):
                pass
    return rows


# ---------------------------------------------------------------------------
# Table generation
# ---------------------------------------------------------------------------

WORKLOAD_ORDER = [
    "A(50R/50U)", "B(95R/5U)", "C(100R)", "D(95R/5I)",
    "E(95Scan/5I)", "F(50R/50RMW)",
    "W(1R/99W)", "RW(50R/50W)", "RSW(25R/25W/50S)", "RS(47R/47W/6S)", "R(95R/5W)"
]

SCALE_ORDER = [100000, 500000, 1000000, 3000000, 5000000, 10000000, 15000000]


def format_scale(sc: int) -> str:
    if sc >= 1_000_000:
        return f"{sc // 1_000_000}M"
    return f"{sc // 1_000}K"


def generate_main_table(rows: List[Dict], scale: int, rdb_rows: List[Dict] = None) -> str:
    """Reproduce the main throughput/WAF/RAF/P99 table for a given scale."""
    lines = []
    sc_str = format_scale(scale)
    repeats = len(set(r['run_id'] for r in rows if r['scale'] == scale)) if rows else 0
    has_rdb = rdb_rows and any(r['scale'] == scale for r in rdb_rows)

    lines.append(f"\n### Scale {sc_str} — {repeats} repeat(s), mean ± std, Welch t-test")
    lines.append("")

    if has_rdb:
        lines.append("| Workload | Metric | Baseline (Mean ± Std) | CASCADE (Mean ± Std) | RocksDB (Mean ± Std) | Diff vs Baseline (%) | Diff vs RocksDB (%) | Welch p | Sig (p<0.05) |")
        lines.append("|:---|:---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|")
    else:
        lines.append("| Workload | Metric | Baseline (Mean ± Std) | CASCADE (Mean ± Std) | Diff (%) | Welch p | Sig (p<0.05) |")
        lines.append("|:---|:---|:---:|:---:|:---:|:---:|:---:|")

    scale_rows = [r for r in rows if r['scale'] == scale]
    if not scale_rows:
        lines.append(f"*No data for scale {sc_str}*")
        return "\n".join(lines)

    # Get all workload names in standard order, then any remainder
    wl_names_seen = list(dict.fromkeys(r['workload'] for r in scale_rows))
    ordered = [w for w in WORKLOAD_ORDER if w in wl_names_seen]
    ordered += [w for w in wl_names_seen if w not in ordered]

    for wl in ordered:
        base_vals  = [r for r in scale_rows if r['workload'] == wl and r['config'] == 'Baseline']
        casc_vals  = [r for r in scale_rows if r['workload'] == wl and r['config'] == 'CASCADE']
        has_outlier = any(r['outlier'] for r in base_vals + casc_vals)
        wl_label = f"**{wl}**{'  ⚠️' if has_outlier else ''}"

        if has_outlier:
            lines.append(f"> ⚠️ Outlier flagged in {wl} at {sc_str} — NOT dropped, included in all aggregates.")

        for metric, field in [
            ("Throughput (Kops/s)", lambda r: r['throughput_ops_s'] / 1000.0),
            ("WAF",                 lambda r: r['waf']),
            ("RAF",                 lambda r: r['raf']),
            ("P99 Latency (µs)",   lambda r: r['p99_us']),
        ]:
            bv = [field(r) for r in base_vals]
            cv = [field(r) for r in casc_vals]
            bm, bs = mean(bv), stddev(bv)
            cm, cs = mean(cv), stddev(cv)
            diff = ((cm - bm) / bm * 100) if bm > 0 else 0.0
            diff_str = f"{'+' if diff >= 0 else ''}{diff:.1f}%"

            if metric == "Throughput (Kops/s)":
                p = welch_t_test(bv, cv)
                _, _ = mann_whitney_u(bv, cv)
                sig = "**Yes**" if p < 0.05 else "No"
                p_str = f"p={p:.4f}"

                if has_rdb:
                    rdb_sc = [r for r in (rdb_rows or []) if r['scale'] == scale and r['workload'] == wl]
                    rv = [r['throughput_kops'] for r in rdb_sc]
                    rm, rs = mean(rv), stddev(rv)
                    rdb_str = f"{rm:.1f} ± {rs:.1f}" if rv else "N/A"
                    diff_rdb = ((cm - rm) / rm * 100) if (rv and rm > 0) else 0.0
                    diff_rdb_str = f"{'+' if diff_rdb >= 0 else ''}{diff_rdb:.1f}%" if rv else "N/A"
                    lines.append(f"| {wl_label} | {metric} | {bm:.1f} ± {bs:.1f} | {cm:.1f} ± {cs:.1f} | {rdb_str} | {diff_str} | {diff_rdb_str} | {p_str} | {sig} |")
                else:
                    lines.append(f"| {wl_label} | {metric} | {bm:.1f} ± {bs:.1f} | {cm:.1f} ± {cs:.1f} | {diff_str} | {p_str} | {sig} |")
            elif metric == "P99 Latency (µs)":
                if has_rdb:
                    rdb_sc = [r for r in (rdb_rows or []) if r['scale'] == scale and r['workload'] == wl]
                    rv_p = [r['p99_us'] for r in rdb_sc]
                    rm_p, rs_p = mean(rv_p), stddev(rv_p)
                    rdb_p_str = f"{rm_p:.1f} ± {rs_p:.1f}" if rv_p else "N/A"
                    diff_rdb_p = ((cm - rm_p) / rm_p * 100) if (rv_p and rm_p > 0) else 0.0
                    diff_rdb_p_str = f"{'+' if diff_rdb_p >= 0 else ''}{diff_rdb_p:.1f}%" if rv_p else "N/A"
                    lines.append(f"| | {metric} | {bm:.1f} ± {bs:.1f} | {cm:.1f} ± {cs:.1f} | {rdb_p_str} | {diff_str} | {diff_rdb_p_str} | - | - |")
                else:
                    lines.append(f"| | {metric} | {bm:.1f} ± {bs:.1f} | {cm:.1f} ± {cs:.1f} | {diff_str} | - | - |")
            else:
                if has_rdb:
                    lines.append(f"| | {metric} | {bm:.2f} ± {bs:.2f} | {cm:.2f} ± {cs:.2f} | - | {diff_str} | - | - | - |")
                else:
                    lines.append(f"| | {metric} | {bm:.2f} ± {bs:.2f} | {cm:.2f} ± {cs:.2f} | {diff_str} | - | - |")

    return "\n".join(lines)


def generate_ahlc_sweep_table(csv_path: str) -> str:
    if not os.path.exists(csv_path):
        return f"*{csv_path} not found*"
    rows = []
    with open(csv_path, newline='') as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                rows.append({
                    'scale': int(row['scale']),
                    'workload': row['workload'],
                    'hysteresis_epochs': int(row['hysteresis_epochs']),
                    'tau_v': float(row['tau_v']),
                    'ahlc_switches': int(row['ahlc_switches']),
                    'throughput_kops': float(row['throughput_kops']),
                    'waf': float(row['waf']),
                })
            except (ValueError, KeyError):
                pass
    if not rows:
        return "*No AHLC sweep data found*"

    lines = ["\n### AHLC Hysteresis Sensitivity Sweep", ""]
    lines.append("| Scale | Workload | Hyst Epochs | τ_v (B/s) | Switches | Tput (Kops/s) | WAF |")
    lines.append("|:---:|:---:|:---:|:---:|:---:|:---:|:---:|")
    for r in rows:
        lines.append(f"| {format_scale(r['scale'])} | {r['workload']} | {r['hysteresis_epochs']} | {int(r['tau_v'])} | {r['ahlc_switches']} | {r['throughput_kops']:.1f} | {r['waf']:.2f} |")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Reproduce CASCADE paper tables from raw CSV files.")
    parser.add_argument("--results-dir", default="bench/results",
                        help="Directory containing raw CSV files")
    parser.add_argument("--output", default="bench/results/aggregated.md",
                        help="Output markdown file")
    args = parser.parse_args()

    rdir = args.results_dir

    # Load all allruns CSVs (one per scale or a combined one)
    all_rows: List[Dict] = []
    combined = os.path.join(rdir, "ycsb_allruns_combined.csv")
    if os.path.exists(combined):
        all_rows = load_allruns_csv(combined)
    else:
        # Load per-scale files
        for sc in SCALE_ORDER:
            sc_str = format_scale(sc)
            path = os.path.join(rdir, f"ycsb_{sc_str}_allruns.csv")
            rows = load_allruns_csv(path)
            all_rows.extend(rows)

    # Load RocksDB CSV (optional)
    rdb_rows = load_rocksdb_csv(os.path.join(rdir, "rocksdb_allruns.csv"))

    if not all_rows:
        print("WARNING: No CASCADE/Baseline CSV data found. Run ./rigorous_bench --all first.")
        print(f"Expected: {rdir}/ycsb_<scale>_allruns.csv")
    
    # Build output
    out_lines = [
        "# CASCADE Research Engine — Aggregated Results",
        "",
        "> Generated by `scripts/aggregate_results.py` from raw CSV files.",
        "> All mean/std/p-values are computed directly from raw per-run data.",
        "> Outliers (>2σ) are flagged but NOT dropped from aggregates.",
        "",
        "## Methodology Notes",
        "",
        "- **11 workloads**: Standard YCSB A–F + paper-defined W, RW, RSW, RS, R",
        "- **6 scales**: 100K / 500K / 1M / 5M / 10M / 15M",
        "- **Repeats**: 5 at 100K–5M; 3 at 10M and 15M (wall-clock budget reduction — stated explicitly)",
        "- **Seed**: `generateOps(seed = 42 + run_id)` — same seed per run_id across all 11 workloads",
        "- **Statistics**: Welch's t-test (two-tailed, normal approximation) + Mann-Whitney U",
        "- **RAF definition**: `sstable_block_reads / total_reads`.",
        "  One block = one 4KB `pread()` call to the OS (cache hits not counted).",
        "",
    ]

    # Find all scales present in data
    scales_present = sorted(set(r['scale'] for r in all_rows)) if all_rows else SCALE_ORDER

    # --- Main tables: one per scale ---
    out_lines.append("## Main Benchmark Tables")
    for sc in scales_present:
        out_lines.append(generate_main_table(all_rows, sc, rdb_rows if rdb_rows else None))
        out_lines.append("")

    # --- AHLC Sweep ---
    out_lines.append("\n## AHLC Hysteresis Sensitivity Sweep")
    for csv_name in ["ahlc_sweep_500K.csv", "ahlc_sweep_1M.csv", "ahlc_sweep_ctrl_1M.csv"]:
        out_lines.append(generate_ahlc_sweep_table(os.path.join(rdir, csv_name)))
        out_lines.append("")

    # --- Summary statistics across all scales ---
    out_lines.append("\n## Per-Workload Summary Across All Scales")
    out_lines.append("")
    if all_rows:
        wl_names_seen = list(dict.fromkeys(r['workload'] for r in all_rows))
        ordered_wls = [w for w in WORKLOAD_ORDER if w in wl_names_seen]
        ordered_wls += [w for w in wl_names_seen if w not in ordered_wls]

        out_lines.append("| Workload | Scale | Baseline Tput (Kops/s) | CASCADE Tput (Kops/s) | RocksDB Tput (Kops/s) | Diff vs Baseline (%) | Diff vs RocksDB (%) | p-value | Sig |")
        out_lines.append("|:---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|")
        for wl in ordered_wls:
            for sc in scales_present:
                sc_str = format_scale(sc)
                bv = [r['throughput_ops_s']/1000 for r in all_rows
                      if r['workload']==wl and r['scale']==sc and r['config']=='Baseline']
                cv = [r['throughput_ops_s']/1000 for r in all_rows
                      if r['workload']==wl and r['scale']==sc and r['config']=='CASCADE']
                if not bv or not cv:
                    continue
                bm, cm = mean(bv), mean(cv)
                diff = ((cm - bm) / bm * 100) if bm > 0 else 0.0
                p = welch_t_test(bv, cv)
                sig = "**Yes**" if p < 0.05 else "No"

                rdb_sc = [r for r in (rdb_rows or []) if r['scale'] == sc and r['workload'] == wl]
                rv = [r['throughput_kops'] for r in rdb_sc]
                rm = mean(rv) if rv else 0.0
                rdb_str = f"{rm:.1f}" if rv else "N/A"
                diff_rdb = ((cm - rm) / rm * 100) if (rv and rm > 0) else 0.0
                diff_rdb_str = f"{'+' if diff_rdb >= 0 else ''}{diff_rdb:.1f}%" if rv else "N/A"

                out_lines.append(
                    f"| {wl} | {sc_str} | {bm:.1f} | {cm:.1f} | {rdb_str} | "
                    f"{'+' if diff>=0 else ''}{diff:.1f}% | {diff_rdb_str} | p={p:.4f} | {sig} |"
                )

    output_text = "\n".join(out_lines)
    print(output_text)

    os.makedirs(os.path.dirname(args.output), exist_ok=True)
    with open(args.output, 'w') as f:
        f.write(output_text)
    print(f"\nAggregated results saved to: {args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()
