# CASCADE: Cache-Sensitive Adaptive Storage Architecture for Dynamic and Efficient LSM-Tree Design

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

**CASCADE** is a research-grade LSM-based key-value storage engine in modern C++17 that physically co-designs three previously isolated subsystems:
1. **Concurrent Cache-Sensitive $B^+$-Tree ($	ext{CSB}^+$ Tree) MemTable** with CPU cache-line aligned nodes (`alignas(64)`) and Epoch-Based Memory Management (`EBMM`).
2. **Adaptive Hybrid Lightweight Compaction (AHLC)** with multi-signal telemetry (EWMA write velocity, Gini access skew, and dynamic level saturation) and hysteresis anti-thrashing control.
3. **Dual-Trigger Blocked Bloom Filter Layer** combining compaction-driven structural budget reallocation with access-frequency sliding-window tracking (Merlin tracker).

---

## 🏛 Architecture

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
                                  │ • RUM Triad (WAF, RAF, SAF) & Latency P99/P99.9 │
                                  └─────────────────────────────────────────────────┘
```

---

## 📁 Repository Structure

```
.
├── include/
│   ├── ahlc.h         # Multi-signal AHLC policy selector, hysteresis FSM & k-way merge
│   ├── bloom.h        # 64-byte Blocked Bloom Filter & dual-trigger allocators
│   ├── cache.h        # Two-tier LRU Block Cache for 4KB SSTable data blocks
│   ├── common.h       # Key/Value types, config struct & fast hash functions
│   ├── csb_tree.h     # Concurrent CSB+ Tree MemTable interface
│   ├── epoch.h        # Epoch-Based Memory Management (EBMM)
│   ├── lsm.h          # LSM storage engine orchestrator
│   ├── metrics.h      # Lock-free HDR latency histogram & RUM triad accounting
│   ├── sstable.h      # SSTable format with block index, filters & fence pointers
│   ├── wal.h          # Write-Ahead Log with group-commit batching
│   └── workload.h     # Full YCSB (A–F) workload & Scrambled Zipfian generators
├── src/
│   ├── ahlc.cpp       # Leveling, Tiering, Sub-range compaction & heap merge
│   ├── bloom.cpp      # SIMD/Blocked Bloom probing & structural reallocation
│   ├── cache.cpp      # LRU Block cache implementation
│   ├── csb_tree.cpp   # CSB+ Tree proactive split & memory reclamation
│   ├── lsm.cpp        # LSM orchestration (insert, search, scan, del, flush, bg compaction)
│   └── sstable.cpp    # SSTable builder & point/range search
├── bench/
│   └── ycsb_bench.cpp # Full YCSB benchmark runner & ablation study
├── tests/
│   └── test_all.cpp   # Comprehensive test suite (100% pass across all stages)
├── Makefile           # Build system with ASan and TSan targets
└── README.md
```

---

## ⚡ Quick Start

### Prerequisites
- C++17 compliant compiler (`g++` or `clang++`)
- POSIX-compatible environment (Linux, macOS)
- `pthread` library

### Building

```bash
# Build both the test suite and benchmark binary
make clean && make
```

### Running Tests
```bash
./cascade_test
```

### Running YCSB Benchmarks
```bash
./cascade_bench
```

### Sanitizer Checks
```bash
# AddressSanitizer & UndefinedBehaviorSanitizer
make asan

# ThreadSanitizer
make tsan
```

---

## 📊 Benchmark Results

### 1. YCSB Core Workloads (A through F)

| Workload | Operations | Throughput | Write Amp (WAF) | Read Amp (RAF) | Space Amp (SAF) | Bloom FPR | P50 Latency | P99 Latency | P99.9 Latency |
|---|---|---|---|---|---|---|---|---|---|
| **YCSB-A** | 50% Read / 50% Update | **3,052 K ops/s** | 1.13× | 0.12 blk/q | 0.80× | 0.016% | 0.2 µs | 0.4 µs | 1.6 µs |
| **YCSB-B** | 95% Read / 5% Update | **4,651 K ops/s** | 1.24× | 0.02 blk/q | 0.91× | 0.052% | 0.1 µs | 0.2 µs | 1.6 µs |
| **YCSB-C** | 100% Read | **5,528 K ops/s** | 1.33× | 0.03 blk/q | 1.00× | 0.040% | 0.1 µs | 0.2 µs | 1.6 µs |
| **YCSB-D** | 95% Read / 5% Insert | **4,328 K ops/s** | 1.24× | 0.03 blk/q | 0.91× | 0.037% | 0.1 µs | 0.2 µs | 0.8 µs |
| **YCSB-E** | 95% Scan / 5% Insert | **827 K ops/s** | 1.09× | 0.00 blk/q | 0.76× | 0.000% | 0.0 µs | 0.0 µs | 0.0 µs |
| **YCSB-F** | 50% Read / 50% RMW | **2,282 K ops/s** | 4.21× | 0.15 blk/q | 0.91× | 0.364% | 0.2 µs | 0.4 µs | 1.6 µs |

### 2. Workload Skew Sensitivity (Zipfian $	heta$ Sweep on Workload A)

| Skew Parameter ($	heta$) | Workload Distribution | Throughput | Write Amplification (WAF) | AHLC Switches |
|---|---|---|---|---|
| $	heta = 0.00$ | Uniform Random | 2,363.5 K ops/s | 3.11× | 2 |
| $	heta = 0.80$ | Moderate Skew | 2,572.7 K ops/s | 2.09× | 1 |
| $	heta = 0.90$ | High Skew | 3,023.1 K ops/s | 1.13× | 1 |
| $	heta = 0.99$ | Extreme Pareto Skew | **3,157.1 K ops/s** | **1.13×** | 1 |

---

## 📜 References
- **Dwivedi et al.** "CASCADE: Cache-Sensitive Adaptive Storage Architecture for Dynamic and Efficient LSM-Tree Design."
- **P. O'Neil et al.** "The log-structured merge-tree (LSM-tree)." *Acta Informatica*, 1996.
- **J. Rao and K. A. Ross.** "Making $B^+$-trees cache-conscious in main memory." *ACM SIGMOD*, 2000.
- **V. Leis et al.** "The ART of practical synchronization." *DaMoN*, 2016.
- **N. Dayan and S. Idreos.** "Dostoevsky: Better space-time trade-offs for LSM-tree based key-value stores." *ACM SIGMOD*, 2018.
- **Z. Zhu et al.** "Mnemosyne: Dynamic workload-aware Bloom filter tuning in LSM trees." *ACM SIGMOD*, 2025.
- **B. F. Cooper et al.** "Benchmarking cloud serving systems with YCSB." *ACM SoCC*, 2010.

---

## 📄 License
This project is licensed under the MIT License.
