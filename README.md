# CASCADE: Cache-Sensitive Adaptive Storage Architecture for Dynamic and Efficient LSM-Tree Design

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

**CASCADE** is a research-grade LSM-based key-value storage engine in modern C++17 that physically co-designs three previously isolated subsystems:
1. **Concurrent Cache-Sensitive $B^+$-Tree ($\text{CSB}^+$ Tree) MemTable** with CPU cache-line aligned nodes (`alignas(64)`) and Epoch-Based Memory Reclamation (`EBMM`).
2. **Adaptive Hybrid Lightweight Compaction (AHLC)** with multi-signal telemetry (EWMA write velocity, Gini access skew, and dynamic level saturation) with hysteresis control.
3. **Dual-Trigger Blocked Bloom Filter Layer** combining compaction-driven structural budget reallocation with access-frequency sliding-window tracking (Merlin tracker).

---

## 🏛 Architecture Overview

```
                                ┌─────────────────────────────────────────────────────────┐
                                │                    CASCADE ENGINE                       │
                                └─────────────────────────────────────────────────────────┘
                                                             │
          ┌─────────────────────────────────────────────────┼─────────────────────────────────────────────────┐
          │                                                 │                                                 │
          ▼                                                 ▼                                                 ▼
┌─────────────────────────────────┐       ┌─────────────────────────────────┐       ┌─────────────────────────────────┐
│     1. CONCURRENT MEMTABLE      │       │     2. SIGNAL-DRIVEN AHLC       │       │    3. DUAL-TRIGGER BLOOM        │
├─────────────────────────────────┤       ├─────────────────────────────────┤       ├─────────────────────────────────┤
│ • Cache-line aligned (64B)      │       │ • EWMA Write Velocity           │       │ • Structural Layer (AHLC event) │
│ • Optimistic Lock Coupling(OLC) │──────>│ • Gini Access Skew              │──────>│ • Frequency Layer (Merlin)      │
│ • Epoch-Based GC (EBMM)         │       │ • Hysteresis State Machine      │       │ • Closed-form budget allocation │
│ • `include/csb_tree.h`          │       │ • `include/ahlc.h`              │       │ • `include/bloom.h`             │
└─────────────────────────────────┘       └─────────────────────────────────┘       └─────────────────────────────────┘
          │                                                 │                                                 │
          └─────────────────────────────────────────────────┼─────────────────────────────────────────────────┘
                                                            ▼
                                  ┌─────────────────────────────────────────────────┐
                                  │      4. STORAGE, EVALUATION & BENCHMARKS        │
                                  ├─────────────────────────────────────────────────┤
                                  │ • SSTable Block Cache & Fence Pointers          │
                                  │ • Write-Ahead Log (WAL) with group commit       │
                                  │ • Full YCSB (A–F) + Scrambled Zipfian Generator │
                                  │ • RUM Triad (WAF, RAF, SAF) & Latency P50/P99    │
                                  └─────────────────────────────────────────────────┘
```

---

## ⚡ Quick Start

### Prerequisites
- **C++17 Compiler:** GCC 13+ or Clang 16+ (Apple Clang 16+)
- **OS:** Linux x86-64 or macOS arm64
- **Libraries:** POSIX `pthread`

### Build & Run
```bash
# 1. Clean build
make clean && make

# 2. Run unit & integration test suite (223 passed, 0 failed)
./cascade_test

# 3. Run full YCSB 10M-ops benchmark suite
./cascade_bench
```

---

## 📊 Benchmark Results (Production 10M Scale)

All tests performed on **N = 10,000,000 operations** (`memtable_capacity = 4096`, `bloom_budget = 2M bits`, `block_cache = 64MB`).

### 1. Head-to-Head Comparison: CASCADE vs Baseline (10,000,000 Ops)

| YCSB Workload | Access Pattern | Baseline Throughput | CASCADE Throughput | Throughput Gain | Baseline WAF | CASCADE WAF | WAF Reduction |
|---|---|---|---|---|---|---|---|
| **YCSB-A** | 50% Read / 50% Update | 370.8 K ops/s | **464.8 K ops/s** | **+25.3%** | 22.36× | **16.28×** | **-27.2%** |
| **YCSB-B** | 95% Read / 5% Update | 738.0 K ops/s | **825.5 K ops/s** | **+11.9%** | 30.65× | **18.71×** | **-39.0%** |
| **YCSB-C** | 100% Read | 432.0 K ops/s | 416.6 K ops/s | -3.6% | 33.21× | **18.90×** | **-43.1%** |
| **YCSB-D** | 95% Read / 5% Insert | 348.9 K ops/s | **370.2 K ops/s** | **+6.1%** | 31.33× | **18.61×** | **-40.6%** |
| **YCSB-E** | 95% Scan / 5% Insert | 132.0 K ops/s | **137.9 K ops/s** | **+4.5%** | 21.39× | **15.93×** | **-25.5%** |
| **YCSB-F** | 50% Read / 50% RMW | 953.1 K ops/s | **974.3 K ops/s** | **+2.2%** | 4.07× | 5.35× | +31.4% |

### Key Performance Summary
- **Throughput:** CASCADE achieves up to **+25.3% throughput advantage** under heavy write/update pressure (YCSB-A).
- **Write Amplification (WAF):** AHLC reduces write amplification by **27% to 43%** across read-heavy and update-heavy workloads by dynamically transitioning levels to tiering mode when write velocity spikes.
- **Tail Latency:** P99 read latency under YCSB-A drops from **6.4 µs** (Baseline) to **3.2 µs** (CASCADE).

---

### 2. Multi-threaded MemTable Throughput Scaling (Workload A, N=100K per thread count)

Genuine comparison between `SkipListMemtable` (shared-mutex, pointer chasing) and `ConcurrentCSBTree` (OLC, 64B cache-line aligned nodes):

| Thread Count | SkipList (K ops/s) | CSB+ Tree (K ops/s) | CSB+ Throughput Advantage |
|---|---|---|---|
| **1 Thread** | 2,391.5 | **2,597.4** | **+8.6%** |
| **2 Threads** | 1,169.6 | **1,360.8** | **+16.3%** |
| **4 Threads** | 694.6 | **862.1** | **+24.1%** |
| **8 Threads** | 508.3 | 362.2 | -28.7% (mutex contention during flush) |
| **16 Threads** | 306.9 | **368.9** | **+20.2%** |

---

### 3. Workload Skew Sensitivity (Zipfian $\theta$ Sweep on Workload A, N=500K)

| Zipfian Parameter ($\theta$) | Workload Distribution | Throughput (K ops/s) | WAF | AHLC Strategy Switches |
|---|---|---|---|---|
| $\theta = 0.00$ | Uniform Random | 797.3 | 10.31× | 26 |
| $\theta = 0.80$ | Moderate Skew | 915.8 | 10.13× | 25 |
| $\theta = 0.90$ | High Skew | 1,083.5 | 7.71× | 23 |
| $\theta = 0.99$ | Extreme Pareto Skew | **1,390.2** | **6.22×** | 21 |

---

### 4. 8-Config Ablation Study (Workload A, N=200K)

| MemTable | Compaction | Bloom Allocation | Throughput | WAF | RAF | SAF | Bloom FPR |
|---|---|---|---|---|---|---|---|
| SkipList | Fixed-Leveling | Uniform Static | 2,051.9 K ops/s | 3.04× | 0.41 | 0.71× | 6.90% |
| SkipList | Fixed-Leveling | Adaptive Dual-Trigger | 2,117.5 K ops/s | 3.04× | 0.29 | 0.71× | **0.77%** |
| SkipList | AHLC | Uniform Static | 2,020.1 K ops/s | 2.93× | 0.53 | 0.70× | 6.47% |
| SkipList | AHLC | Adaptive Dual-Trigger | 2,068.2 K ops/s | 2.93× | 0.37 | 0.70× | **0.81%** |
| CSB+ Tree | Fixed-Leveling | Uniform Static | 2,179.7 K ops/s | 3.04× | 0.41 | 0.71× | 6.90% |
| CSB+ Tree | Fixed-Leveling | Adaptive Dual-Trigger | 2,212.9 K ops/s | 3.04× | 0.29 | 0.71× | **0.77%** |
| CSB+ Tree | AHLC | Uniform Static | 2,126.6 K ops/s | 2.93× | 0.53 | 0.70× | 6.47% |
| **CSB+ Tree** | **AHLC** | **Adaptive Dual-Trigger (CASCADE)** | **2,194.6 K ops/s** | **2.93×** | **0.37** | **0.70×** | **0.81%** |

---

## ⚠️ Known Limitations & Open Engineering Challenges

While CASCADE demonstrates strong empirical gains, the following design trade-offs and remaining limitations exist:

1. **Background Compaction Lock Contention (`levels_mu_`):**
   - Compaction acquisition locks the `levels_mu_` mutex during merges. Under high multi-threaded write scaling (e.g. 8–16 threads), worker threads waiting on memtable flushes experience lock contention on `levels_mu_`. A lock-free level array (e.g. RCU/Copy-On-Write level manifests) would resolve this bottleneck.
2. **In-Memory Storage Simulation:**
   - Data blocks are kept in heap memory with precise byte accounting (`bytes_written_sstables`, `bytes_read_sstables`, `ssd_write_ns_per_byte`). While disk bandwidth and latency costs are faithfully modeled, actual direct I/O syscall overhead (`io_uring`/`O_DIRECT`) is not measured.
3. **Bloom Reallocation CPU Overhead:**
   - Frequent bloom filter bitset reallocations during rapid compactions incur transient heap reallocations. Increasing the hysteresis epoch threshold (`ahlc_hysteresis_epochs`) mitigates this overhead under erratic write bursts.

---

## 📜 References
- **Dwivedi et al.** "CASCADE: Cache-Sensitive Adaptive Storage Architecture for Dynamic and Efficient LSM-Tree Design."
- **P. O'Neil et al.** "The log-structured merge-tree (LSM-tree)." *Acta Informatica*, 1996.
- **J. Rao and K. A. Ross.** "Making $B^+$-trees cache-conscious in main memory." *ACM SIGMOD*, 2000.
- **V. Leis et al.** "The ART of practical synchronization." *DaMoN*, 2016.
- **N. Dayan and S. Idreos.** "Dostoevsky: Better space-time trade-offs for LSM-tree based key-value stores." *ACM SIGMOD*, 2018.

---

## 📄 License
This project is licensed under the MIT License.
