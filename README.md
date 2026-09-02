# CASCADE: Cache-Sensitive Adaptive Storage Architecture for Dynamic and Efficient LSM-Tree Design

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Tests](https://img.shields.io/badge/Tests-234%20passed%2C%200%20failed-brightgreen.svg)]()
[![ASan](https://img.shields.io/badge/ASan-Clean-brightgreen.svg)]()
[![TSan](https://img.shields.io/badge/TSan-Clean-brightgreen.svg)]()

**CASCADE** is a research-grade LSM-based key-value storage engine in modern C++17 featuring real file-backed SSTables, a persistent append-only Write-Ahead Log (WAL) with group commit, and three co-designed subsystems:
1. **Concurrent Cache-Sensitive $B^+$-Tree ($\text{CSB}^+$ Tree) MemTable** with CPU cache-line aligned nodes (`alignas(64)`) and Epoch-Based Memory Reclamation (`EBMM`).
2. **Adaptive Hybrid Lightweight Compaction (AHLC)** with multi-signal telemetry (EWMA write velocity, Gini access skew, and dynamic level saturation) with hysteresis control.
3. **Dual-Trigger Blocked Bloom Filter Layer** combining compaction-driven structural budget reallocation with access-frequency tracking.

> **Scope & Realistic Positioning**: CASCADE is a research prototype designed to evaluate these architectural ideas against an identical from-scratch baseline under controlled conditions. It is **not** a production drop-in replacement for mature systems like RocksDB (which includes a decade of production hardening, parallel multi-threaded compaction, block compression, and extensive tooling).

---

## 🏛 Architecture Overview

```
                                ┌─────────────────────────────────────────────────────────┐
                                │                    CASCADE ENGINE                       │
                                └─────────────────────────────────────────────────────────┘
                                                             │
          ┌──────────────────────────────────────────────────┼─────────────────────────────────────────────────┐
          │                                                  │                                                 │
          ▼                                                  ▼                                                 ▼
┌─────────────────────────────────┐        ┌─────────────────────────────────┐        ┌─────────────────────────────────┐
│     1. CONCURRENT MEMTABLE      │        │     2. SIGNAL-DRIVEN AHLC       │        │    3. DUAL-TRIGGER BLOOM        │
├─────────────────────────────────┤        ├─────────────────────────────────┤        ├─────────────────────────────────┤
│ • Cache-line aligned (64B)      │        │ • EWMA Write Velocity           │        │ • Structural Layer (AHLC event) │
│ • Optimistic Lock Coupling(OLC) │───────>│ • Gini Access Skew              │───────>│ • Frequency Layer (heat track)  │
│ • Epoch-Based GC (EBMM)         │        │ • Hysteresis State Machine      │        │ • Monkey-optimal per-key sizing │
│ • `include/csb_tree.h`          │        │ • `include/ahlc.h`              │        │ • `include/bloom.h`             │
└─────────────────────────────────┘        └─────────────────────────────────┘        └─────────────────────────────────┘
          │                                                  │                                                 │
          └──────────────────────────────────────────────────┼─────────────────────────────────────────────────┘
                                                             ▼
                                   ┌─────────────────────────────────────────────────┐
                                   │      4. REAL DISK STORAGE & PERSISTENCE         │
                                   ├─────────────────────────────────────────────────┤
                                   │ • File-backed SSTables with 4KB Data Blocks     │
                                   │ • Block Cache (LRU) & 28-byte Index Entries     │
                                   │ • Persistent append-only WAL (`fsync` batches)  │
                                   │ • Full Crash Recovery & WAL Replay              │
                                   └─────────────────────────────────────────────────┘
```

---

## ⚡ Quick Start

### Prerequisites
- **C++17 Compiler:** GCC 13+ or Clang 16+ (Apple Clang 16+ on Apple Silicon)
- **OS:** macOS arm64 or Linux x86-64
- **Libraries:** POSIX `pthread`

### Build & Run
```bash
# 1. Clean build
make clean && make

# 2. Run test suite (234 passed, 0 failed — ASan/TSan verified)
make test

# 3. Run rigorous benchmark sequence across 100K -> 500K -> 1M -> 5M ops
./rigorous_bench --scale 100000 --repeats 3 --single
./rigorous_bench --scale 500000 --repeats 3 --single
./rigorous_bench --scale 1000000 --repeats 3 --single
./rigorous_bench --scale 5000000 --repeats 1 --single

# 4. Profile the two key engineering anomalies
./rigorous_bench --anomaly1
./rigorous_bench --anomaly2
```

---

## 📊 Real File-Backed Benchmark Results

All measurements come from reproducible runs writing real binary SSTables and WAL files to disk with single-threaded I/O (`fsync` group commits per 64 writes, 4KB SSTable data blocks, 64MB LRU BlockCache).

### 1. Scale Comparison: 1M Operations (3 Repeats, Mean ± Std)
*Command: `./rigorous_bench --scale 1000000 --repeats 3 --single`*

| Workload | Access Pattern | Baseline Throughput (Kops/s) | CASCADE Throughput (Kops/s) | Throughput Diff | Baseline WAF | CASCADE WAF | WAF Reduction | Mann-Whitney U | Welch t-test $p$ |
|---|---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| **YCSB-A** | 50% Read / 50% Update | 413.1 ± 3.1 | 376.2 ± 7.1 | -8.9% | 10.78 | 12.16 | +12.8% (worse) | U=0 | $p < 0.001$ |
| **YCSB-B** | 95% Read / 5% Update | 1017.0 ± 56.6 | **1238.4 ± 10.3** | **+21.8%** | 16.49 | **11.97** | **-27.4%** | U=0 | $p < 0.001$ |
| **YCSB-C** | 100% Read | 1493.4 ± 78.3 | **1648.3 ± 42.8** | **+10.4%** | 15.95 | **12.51** | **-21.6%** | U=0 | $p = 0.003$ |
| **YCSB-D** | 95% Read / 5% Insert | 833.7 ± 10.3 | **918.4 ± 22.6** | **+10.2%** | 16.64 | **12.57** | **-24.4%** | U=0 | $p < 0.001$ |
| **YCSB-E** | 95% Scan / 5% Insert | 79.3 ± 2.9 | **95.2 ± 2.6** | **+20.0%** | 10.43 | 11.90 | +14.2% (worse) | U=0 | $p < 0.001$ |
| **YCSB-F** | 50% Read / 50% RMW | 412.7 ± 15.7 | 374.1 ± 3.6 | -9.3% | 10.78 | 12.16 | +12.8% (worse) | U=0 | $p < 0.001$ |

---

### 2. Large Scale: 5,000,000 Operations (Single Run)
*Command: `./rigorous_bench --scale 5000000 --repeats 1 --single`*  
*(Note: Explicitly labeled as a single run due to execution duration; no artificial error bars.)*

| Workload | Access Pattern | Baseline Tput (Kops/s) | CASCADE Tput (Kops/s) | Tput Diff | Baseline WAF | CASCADE WAF | WAF Reduction | AHLC Switches |
|---|---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| **YCSB-A** | 50% Read / 50% Update | 61.0 | **79.5** | **+30.3%** | 82.65 | **42.79** | **-48.2%** | 226 |
| **YCSB-B** | 95% Read / 5% Update | 563.1 | 521.5 | -7.4% | 53.33 | **32.23** | **-39.6%** | 145 |
| **YCSB-C** | 100% Read | 1119.2 | **1149.8** | **+2.7%** | 58.15 | **26.98** | **-53.6%** | 138 |
| **YCSB-D** | 95% Read / 5% Insert | 372.4 | **450.0** | **+20.8%** | 55.84 | **26.22** | **-53.0%** | 153 |
| **YCSB-E** | 95% Scan / 5% Insert | 31.5 | **39.5** | **+25.4%** | 81.31 | **42.38** | **-47.9%** | 235 |
| **YCSB-F** | 50% Read / 50% RMW | 58.8 | **77.0** | **+31.0%** | 82.65 | **42.79** | **-48.2%** | 226 |

---

## 🔍 Detailed Analysis of the Two Engineering Anomalies

CASCADE does **not** win across the board. Detailed profiling reveals the exact architectural trade-offs:

### Anomaly 1: CSB+ Tree Regressing Under 16 Concurrently Contending Writers
*Command: `./rigorous_bench --anomaly1` (N = 160,000 ops across 16 threads)*

```
Data Structure        Throughput       Total Lock Wait    Acquisitions    Avg Wait/Lock
ConcurrentCSBTree     472.5 Kops/s     4,857.6 ms         160,000         30.36 µs
SkipListMemtable      595.0 Kops/s     3,842.9 ms         160,000         24.02 µs
```

**Root Mechanism**: The Concurrent CSB+ Tree arranges keys contiguously in 64-byte cache-line aligned node groups to maximize CPU L1/L2 cache locality during binary search. However, during node splits, the writer thread must allocate new contiguous child arrays (`allocGroup`) and perform memory copies while holding the exclusive lock (`rw_mu_`). In contrast, a SkipList merely swings a few pointer links without contiguous reallocations. Under 16 concurrent writer threads, this structural overhead increases average lock acquisition wait time by **26.4%** (30.36 µs vs 24.02 µs), causing SkipList to achieve higher raw concurrent ingestion throughput.

---

### Anomaly 2: Workload F (Read-Modify-Write) WAF Regression
*Command: `./rigorous_bench --anomaly2` (N = 100,000 ops, Workload F)*

```
# Workload F Strategy Switches (First 64 ms)
Timestamp(ms)    FromStrategy    ToStrategy    WriteVelocity(B/s)
12.57 ms         HYBRID          TIERING       15.07 MB/s
20.24 ms         TIERING         HYBRID        16.27 MB/s
32.17 ms         HYBRID          TIERING       18.73 MB/s
39.75 ms         TIERING         HYBRID        17.77 MB/s
56.61 ms         HYBRID          TIERING       16.09 MB/s
64.10 ms         TIERING         HYBRID        16.93 MB/s
```

**Root Mechanism**: In Workload F (50% Read, 50% RMW), user transactions continuously alternate between read probes and atomic write updates. The EWMA write velocity tracker experiences volatile fluctuations right around the strategy threshold. This causes AHLC to thrash between `HYBRID` and `TIERING` (6 strategy switches in under 65 ms). When switching from Tiering back to Leveling, AHLC triggers cascading merges of accumulated runs, rewriting keys repeatedly compared to the steady, monotonic merge cadence of fixed leveling.

---

## 📜 References
- **P. O'Neil et al.** "The log-structured merge-tree (LSM-tree)." *Acta Informatica*, 1996.
- **J. Rao and K. A. Ross.** "Making $B^+$-trees cache-conscious in main memory." *ACM SIGMOD*, 2000.
- **N. Dayan and S. Idreos.** "Dostoevsky: Better space-time trade-offs for LSM-tree based key-value stores." *ACM SIGMOD*, 2018.

## 📄 License
MIT License.
