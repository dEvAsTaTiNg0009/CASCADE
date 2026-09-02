# CASCADE: Concurrent Adaptive Scalable Compaction with Adaptive Data Engine

> **A Plain-Language Research Guide** — designed so anyone with a basic understanding of software can understand what this project does, why it matters, and what it achieves.

---

## Table of Contents

1. [What Problem Does This Solve?](#1-what-problem-does-this-solve)
2. [Background: How Data Storage Works](#2-background-how-data-storage-works)
3. [The CASCADE Engine — What It Is](#3-the-cascade-engine--what-it-is)
4. [System Architecture](#4-system-architecture)
5. [The Five Core Modules](#5-the-five-core-modules)
6. [How It All Works Together](#6-how-it-all-works-together)
7. [Benchmark Results & Statistical Evaluation](#7-benchmark-results--statistical-evaluation)
8. [Analysis of the Two Key Engineering Anomalies](#8-analysis-of-the-two-key-engineering-anomalies)
9. [Research Gaps & Publication Readiness](#9-research-gaps--publication-readiness)
10. [File Map](#10-file-map)
11. [How to Build and Run](#11-how-to-build-and-run)

---

## 1. What Problem Does This Solve?

### The simple version

Imagine a warehouse storing millions of boxes (database records). Every time you add a box, you must **re-organize shelves** to keep everything findable. This re-organization — called **compaction** — is expensive. The more boxes you have, the harder it gets.

The challenge: **you don't know in advance which boxes people want most**, and the best re-organization strategy for "mostly adding boxes" is completely different from "mostly searching." Picking the wrong strategy wastes enormous disk I/O.

### The technical version

Modern databases (NoSQL, time-series, analytics) use **LSM-trees** (Log-Structured Merge-trees) — the same structure powering Google Bigtable, Apache Cassandra, RocksDB, and LevelDB. LSM-trees have a fundamental tension:

| Strategy | Write Speed | Read Speed | Disk Usage |
|----------|------------|------------|------------|
| **Tiering** (accumulate first, merge later) | ✅ Fast | ❌ Slow | ❌ High |
| **Leveling** (merge immediately) | ❌ Slow | ✅ Fast | ✅ Low |
| **Hybrid** | ⚖️ Balanced | ⚖️ Balanced | ⚖️ Balanced |

**The problem**: production workloads shift between phases — high write bursts, skewed hot reads, uniform cold access. A static strategy is always suboptimal.

**CASCADE's answer**: automatically switch strategies in real time based on live telemetry, without human tuning.

---

## 2. Background: How Data Storage Works

### The RUM Conjecture

Any storage system must trade off between three costs (proved by Athanassoulis et al., CIDR 2016):

- **R**ead amplification factor (**RAF**) — how many disk blocks must be read per query
- **U**pdate (write) amplification factor (**WAF**) — how many bytes are written per byte inserted
- **M**emory/space amplification factor (**SAF**) — how much physical storage used per logical byte

You **cannot minimize all three simultaneously**. Every design choice is a trade-off. CASCADE's claim is that by dynamically choosing, it reduces the *average* cost across shifting real workloads.

### LSM-tree basics

```
[ User Write ] → [ MemTable (RAM) ] --flush--> [ L0 SSTables (Disk) ] --compact--> [ L1 ] → … → [ Ln ]
```

- **MemTable**: small, fast, in-RAM sorted structure. Receives all writes.
- **SSTable**: immutable sorted file on disk. Created when MemTable fills.
- **Compaction**: merging SSTables to reduce read overhead and reclaim space.
- **Bloom Filter**: probabilistic check — "is key K *possibly* in this level?" — avoids most disk reads.

---

## 3. The CASCADE Engine — What It Is

CASCADE is a **research prototype** of an LSM storage engine in C++17 that combines five novel components:

```
┌──────────────────────────────────────────────────────────────────────┐
│                          CASCADE Engine                               │
│                                                                       │
│  ┌────────────────┐  ┌──────────┐  ┌──────────────────────────────┐  │
│  │ MemTable        │  │   WAL    │  │  AHLC Strategy Selector      │  │
│  │ ConcurrentCSB+  │→ │ (real    │  │  V_w = EWMA write velocity   │  │
│  │  (OLC, epoch-GC)│  │  append  │  │  K_skew = Gini coefficient   │  │
│  │ OR SkipListMT   │  │  log)    │  │  Hysteresis FSM (cooldown)   │  │
│  └────────────────┘  └──────────┘  └──────────────────────────────┘  │
│         ↓ flush                              ↓ directs                │
│  ┌──────────────────────────────────────────────────────────────────┐ │
│  │  Levels: L0 → L1 → L2 → L3 → L4 → L5 → L6 (real disk SSTables)  │ │
│  └──────────────────────────────────────────────────────────────────┘ │
│                   ↑ rebuild after compaction                          │
│  ┌───────────────────────────┐   ┌────────────────────────────────┐   │
│  │ Dual-Trigger Bloom Filter  │   │  LRU Block Cache (64 MB)      │   │
│  │ Structural + Frequency     │   │  Epoch-GC safe memory free    │   │
│  └───────────────────────────┘   └────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────────────┘
```

> **Scope & Realistic Positioning**: CASCADE is a research prototype designed to evaluate these architectural ideas against an identical from-scratch baseline under controlled conditions. It is **not** a production drop-in replacement for mature systems like RocksDB (which includes a decade of production hardening, parallel multi-threaded compaction, block compression, and extensive tooling).

---

## 4. System Architecture

### Write path

```
insert(key, value)
    │
    ├─ WAL.append()           → append-only disk log with 64-write group commit (plain fsync)
    ├─ MemTable.insert()      → ConcurrentCSBTree (OLC) or SkipListMemtable
    └─ if MemTable.isFull()   → flush()
           │
           └─ doFlush()       → writes real binary SSTable file (L0_<id>.sst) to disk
                  │
                  └─ requestCompaction() → wake background thread
                         │
                         └─ doCompaction()
                                │ reads V_w (write velocity) and K_skew (Gini)
                                ├─ Strategy::LEVELING  → compactLeveling()
                                ├─ Strategy::TIERING   → compactTiering()
                                └─ Strategy::HYBRID    → tiering upper + leveling bottom
```

### Read path

```
search(key)
    │
    ├─ 1. MemTable search (lock-free or shared_lock) — O(log N)
    │        → found? return value (or tombstone → false)
    │
    └─ 2. Level scan L0 → L6
             │
             ├─ BloomFilter[i].possiblyContains(key)?
             │     → NO  → skip (true negative, zero disk I/O)
             │     → YES → probe SSTables (newest first)
             │                │
             │                ├─ Check SSTable Bloom Filter
             │                ├─ Binary search IndexEntry array (28 bytes each)
             │                └─ Load 4KB Data Block via LRU BlockCache / pread()
             │
             └─ return value or false (not found)
```

---

## 5. The Five Core Modules

### Module 1: ConcurrentCSBTree (MemTable)
**Files**: [`include/csb_tree.h`](include/csb_tree.h), [`src/csb_tree.cpp`](src/csb_tree.cpp)
Stores child nodes contiguously in memory (`alignas(64)`), eliminating pointer dereferences during inner node traversal. Readers traverse lock-free via version validation (Optimistic Lock Coupling).

### Module 2: AHLC — Adaptive Hybrid Lightweight Compaction
**Files**: [`include/ahlc.h`](include/ahlc.h), [`src/ahlc.cpp`](src/ahlc.cpp)
Multi-signal feedback loop (EWMA write velocity $V_w$ and Gini access skew $K_{skew}$) directing compactions between Tiering, Leveling, and Hybrid states with hysteresis hold.

### Module 3: Blocked Bloom Filter Layer
**Files**: [`include/bloom.h`](include/bloom.h), [`src/bloom.cpp`](src/bloom.cpp)
Blocked Bloom filters constrain hash probes to a single 64-byte cache line (512 bits). Sized with Monkey-optimal depth weighting ($1 + 0.1 \times \text{level}$) and dynamic heat allocation.

### Module 4: Real Disk SSTables & Block Cache
**Files**: [`include/sstable.h`](include/sstable.h), [`src/sstable.cpp`](src/sstable.cpp), [`include/cache.h`](include/cache.h)
File-backed SSTables with 4KB binary data blocks, packed 28-byte index entries, serialized Bloom filters, and 32-byte footers (`magic: 0xCA5CADE0F11E`). Reads leverage the OS page cache via `pread()` and a 64MB LRU `BlockCache`.

### Module 5: Persistent WAL with Group Commit
**Files**: [`include/wal.h`](include/wal.h)
Append-only log file (`wal.log`) buffering up to 64 records per batch, issuing plain `fsync()` once per batch. Replays on startup for crash recovery.

---

## 6. How It All Works Together

- Under heavy write bursts ($V_w > \tau_{write}$), AHLC dynamically transitions levels to **Tiering**, minimizing write stalls and deferring merges.
- Under hot-key skew ($K_{skew} > \tau_{skew}$), AHLC transitions to **Leveling**, condensing runs into single sorted files per level to eliminate read amplification.
- MemTable flushes write directly to disk as `L0_<id>.sst`. Compactions merge SSTables via $k$-way heap merge, write new target-level SSTables, and safely unlink old files.

---

## 7. Benchmark Results & Statistical Evaluation

### Platform
- **Machine**: Apple Silicon (arm64, Darwin 25.6.0)
- **Compiler**: Apple Clang, `-std=c++17 -O2 -pthread`
- **I/O Subsystem**: Real POSIX disk I/O, plain `fsync()`, 4KB block size, 64MB block cache
- **All correctness tests**: **234/234 ✅ PASS** (TSan-clean, ASan-clean)

### Correctness Test Summary

| Test Suite | What It Checks | Result |
|---|---|:---:|
| 1. CSB+ Tree | 2,000 inserts · search · tombstone · range scan · sorted flush | ✅ PASS |
| 2. Bloom Filter | 10K elements · 100K negatives · FPR = **0.24%** (< 5%) | ✅ PASS |
| 3. AHLC Engine | Strategy switching · hysteresis hold · Gini coefficient | ✅ PASS |
| 4. LSM Integrity | 5K keys · 10% delete · compaction · zero ghost reads · WAF ≥ 1.0 | ✅ PASS |
| 5. Concurrent Stress | 8 writers + 8 readers · 16K concurrent ops · TSan-clean | ✅ PASS |
| 6. Per-Key Bloom Sizing | 1M keys · 14 bits/key · measured FPR = **0.365%** (< 1%) | ✅ PASS |
| 7. WAL Persistence & Recovery | Simulated crash (un-flushed memtable) · WAL replay · SSTable persistence | ✅ PASS |

---

### Scale Benchmark 1: 100K Operations (3 Repeats, Mean ± Std)
*Command: `./rigorous_bench --scale 100000 --repeats 3 --single`*

| Workload | Metric | Baseline (Mean ± Std) | CASCADE (Mean ± Std) | Diff (%) | Mann-Whitney U | Welch t-test $p$ | Significance ($p < 0.05$) |
|:---|:---|:---:|:---:|:---:|:---:|:---:|:---:|
| **A (50R/50U)** | Throughput (Kops/s) | 1065.5 ± 13.7 | 1054.3 ± 33.1 | -1.0% | U=4 | $p=0.589$ | Not significant |
| | WAF | 2.95 ± 0.00 | 3.00 ± 0.00 | +1.7% | - | - | - |
| | RAF | 0.22 ± 0.00 | 0.24 ± 0.00 | +9.5% | - | - | - |
| | P99 Latency (µs) | 1.6 ± 0.0 | 1.6 ± 0.0 | +0.0% | - | - | - |
| **B (95R/5U)** | Throughput (Kops/s) | 2421.0 ± 16.0 | **2981.2 ± 24.8** | **+23.1%** | U=0 | **$p < 0.001$** | **Significant (t-test)** |
| | WAF | 3.69 ± 0.00 | 3.52 ± 0.00 | -4.6% | - | - | - |
| | RAF | 0.06 ± 0.00 | 0.08 ± 0.00 | +30.1% | - | - | - |
| | P99 Latency (µs) | 1.6 ± 0.0 | 1.6 ± 0.0 | +0.0% | - | - | - |
| **C (100R)** | Throughput (Kops/s) | 3091.3 ± 33.8 | 3090.9 ± 9.7 | -0.0% | U=3 | $p=0.985$ | Not significant |
| | WAF | 3.95 ± 0.00 | 3.76 ± 0.00 | -4.7% | - | - | - |
| | RAF | 0.10 ± 0.00 | 0.12 ± 0.00 | +29.0% | - | - | - |
| | P99 Latency (µs) | 1.6 ± 0.0 | 1.6 ± 0.0 | +0.0% | - | - | - |
| **D (95R/5I)** | Throughput (Kops/s) | 2020.3 ± 33.3 | **2344.3 ± 23.5** | **+16.0%** | U=0 | **$p < 0.001$** | **Significant (t-test)** |
| | WAF | 3.77 ± 0.00 | 3.60 ± 0.00 | -4.5% | - | - | - |
| | RAF | 0.10 ± 0.00 | 0.13 ± 0.00 | +23.2% | - | - | - |
| | P99 Latency (µs) | 1.6 ± 0.0 | 1.6 ± 0.0 | +0.0% | - | - | - |
| **E (95Scan/5I)** | Throughput (Kops/s) | 130.2 ± 6.8 | **180.9 ± 2.5** | **+38.9%** | U=0 | **$p < 0.001$** | **Significant (t-test)** |
| | WAF | 3.08 ± 0.00 | 2.95 ± 0.00 | -4.2% | - | - | - |
| | RAF | 0.00 ± 0.00 | 0.00 ± 0.00 | +0.0% | - | - | - |
| | P99 Latency (µs) | 0.0 ± 0.0 | 0.0 ± 0.0 | +0.0% | - | - | - |
| **F (50R/50RMW)**| Throughput (Kops/s) | 1070.4 ± 32.6 | 997.8 ± 89.9 | -6.8% | U=2 | $p=0.189$ | Not significant |
| | WAF | 2.95 ± 0.00 | 3.00 ± 0.00 | +1.7% | - | - | - |
| | RAF | 0.22 ± 0.00 | 0.24 ± 0.00 | +9.5% | - | - | - |
| | P99 Latency (µs) | 1.6 ± 0.0 | 1.6 ± 0.0 | +0.0% | - | - | - |

---

### Scale Benchmark 2: 500K Operations (3 Repeats, Mean ± Std)
*Command: `./rigorous_bench --scale 500000 --repeats 3 --single`*

| Workload | Metric | Baseline (Mean ± Std) | CASCADE (Mean ± Std) | Diff (%) | Mann-Whitney U | Welch t-test $p$ | Significance ($p < 0.05$) |
|:---|:---|:---:|:---:|:---:|:---:|:---:|:---:|
| **A (50R/50U)** | Throughput (Kops/s) | 621.0 ± 9.9 | 564.5 ± 5.9 | -9.1% | U=0 | **$p < 0.001$** | **Baseline faster** |
| | WAF | 6.34 ± 0.00 | 7.77 ± 0.00 | +22.7% | - | - | - |
| | RAF | 0.40 ± 0.00 | 0.48 ± 0.00 | +21.0% | - | - | - |
| | P99 Latency (µs) | 3.2 ± 0.0 | 3.2 ± 0.0 | +0.0% | - | - | - |
| **B (95R/5U)** | Throughput (Kops/s) | 1315.1 ± 17.2 | **1550.2 ± 24.8** | **+17.9%** | U=0 | **$p < 0.001$** | **Significant (t-test)** |
| | WAF | 9.11 ± 0.00 | **8.16 ± 0.00** | **-10.5%** | - | - | - |
| | RAF | 0.24 ± 0.00 | 0.25 ± 0.00 | +4.0% | - | - | - |
| | P99 Latency (µs) | 1.6 ± 0.0 | 1.6 ± 0.0 | +0.0% | - | - | - |
| **C (100R)** | Throughput (Kops/s) | 1646.5 ± 3.1 | 1652.8 ± 28.0 | +0.4% | U=3 | $p=0.695$ | Not significant |
| | WAF | 9.14 ± 0.00 | **8.48 ± 0.00** | **-7.2%** | - | - | - |
| | RAF | 0.30 ± 0.00 | 0.33 ± 0.00 | +11.2% | - | - | - |
| | P99 Latency (µs) | 1.6 ± 0.0 | 1.6 ± 0.0 | +0.0% | - | - | - |
| **D (95R/5I)** | Throughput (Kops/s) | 1048.4 ± 10.0 | **1164.2 ± 43.3** | **+11.0%** | U=0 | **$p < 0.001$** | **Significant (t-test)** |
| | WAF | 9.23 ± 0.00 | **8.20 ± 0.00** | **-11.1%** | - | - | - |
| | RAF | 0.28 ± 0.00 | 0.32 ± 0.00 | +14.3% | - | - | - |
| | P99 Latency (µs) | 1.6 ± 0.0 | 1.6 ± 0.0 | +0.0% | - | - | - |
| **E (95Scan/5I)** | Throughput (Kops/s) | 93.8 ± 0.9 | **119.6 ± 1.5** | **+27.5%** | U=0 | **$p < 0.001$** | **Significant (t-test)** |
| | WAF | 6.11 ± 0.00 | 7.48 ± 0.00 | +22.4% | - | - | - |
| | RAF | 0.00 ± 0.00 | 0.00 ± 0.00 | +0.0% | - | - | - |
| | P99 Latency (µs) | 0.0 ± 0.0 | 0.0 ± 0.0 | +0.0% | - | - | - |
| **F (50R/50RMW)**| Throughput (Kops/s) | 616.7 ± 2.4 | 569.3 ± 3.5 | -7.7% | U=0 | **$p < 0.001$** | **Baseline faster** |
| | WAF | 6.34 ± 0.00 | 7.77 ± 0.00 | +22.7% | - | - | - |
| | RAF | 0.40 ± 0.00 | 0.48 ± 0.00 | +21.0% | - | - | - |
| | P99 Latency (µs) | 3.2 ± 0.0 | 3.2 ± 0.0 | +0.0% | - | - | - |

---

### Scale Benchmark 3: 1M Operations (3 Repeats, Mean ± Std)
*Command: `./rigorous_bench --scale 1000000 --repeats 3 --single`*

| Workload | Metric | Baseline (Mean ± Std) | CASCADE (Mean ± Std) | Diff (%) | Mann-Whitney U | Welch t-test $p$ | Significance ($p < 0.05$) |
|:---|:---|:---:|:---:|:---:|:---:|:---:|:---:|
| **A (50R/50U)** | Throughput (Kops/s) | 413.1 ± 3.1 | 376.2 ± 7.1 | -8.9% | U=0 | **$p < 0.001$** | **Baseline faster** |
| | WAF | 10.78 ± 0.00 | 12.16 ± 0.00 | +12.8% | - | - | - |
| | RAF | 0.43 ± 0.00 | 0.51 ± 0.00 | +20.7% | - | - | - |
| | P99 Latency (µs) | 3.2 ± 0.0 | 3.2 ± 0.0 | +0.0% | - | - | - |
| **B (95R/5U)** | Throughput (Kops/s) | 1017.0 ± 56.6 | **1238.4 ± 10.3** | **+21.8%** | U=0 | **$p < 0.001$** | **Significant (t-test)** |
| | WAF | 16.49 ± 0.00 | **11.97 ± 0.00** | **-27.4%** | - | - | - |
| | RAF | 0.28 ± 0.00 | 0.29 ± 0.00 | +5.4% | - | - | - |
| | P99 Latency (µs) | 3.2 ± 0.0 | 1.6 ± 0.0 | -50.0% | - | - | - |
| **C (100R)** | Throughput (Kops/s) | 1493.4 ± 78.3 | **1648.3 ± 42.8** | **+10.4%** | U=0 | **$p = 0.003$** | **Significant (t-test)** |
| | WAF | 15.95 ± 0.00 | **12.51 ± 0.00** | **-21.6%** | - | - | - |
| | RAF | 0.23 ± 0.00 | 0.26 ± 0.00 | +10.8% | - | - | - |
| | P99 Latency (µs) | 1.6 ± 0.0 | 1.6 ± 0.0 | +0.0% | - | - | - |
| **D (95R/5I)** | Throughput (Kops/s) | 833.7 ± 10.3 | **918.4 ± 22.6** | **+10.2%** | U=0 | **$p < 0.001$** | **Significant (t-test)** |
| | WAF | 16.64 ± 0.00 | **12.57 ± 0.00** | **-24.4%** | - | - | - |
| | RAF | 0.28 ± 0.00 | 0.33 ± 0.00 | +16.7% | - | - | - |
| | P99 Latency (µs) | 3.2 ± 0.0 | 3.2 ± 0.0 | +0.0% | - | - | - |
| **E (95Scan/5I)** | Throughput (Kops/s) | 79.3 ± 2.9 | **95.2 ± 2.6** | **+20.0%** | U=0 | **$p < 0.001$** | **Significant (t-test)** |
| | WAF | 10.43 ± 0.00 | 11.90 ± 0.00 | +14.2% | - | - | - |
| | RAF | 0.00 ± 0.00 | 0.00 ± 0.00 | +0.0% | - | - | - |
| | P99 Latency (µs) | 0.0 ± 0.0 | 0.0 ± 0.0 | +0.0% | - | - | - |
| **F (50R/50RMW)**| Throughput (Kops/s) | 412.7 ± 15.7 | 374.1 ± 3.6 | -9.3% | U=0 | **$p < 0.001$** | **Baseline faster** |
| | WAF | 10.78 ± 0.00 | 12.16 ± 0.00 | +12.8% | - | - | - |
| | RAF | 0.43 ± 0.00 | 0.51 ± 0.00 | +20.7% | - | - | - |
| | P99 Latency (µs) | 3.2 ± 0.0 | 3.2 ± 0.0 | +0.0% | - | - | - |

---

### Scale Benchmark 4: 5M Operations (Single Run)
*Command: `./rigorous_bench --scale 5000000 --repeats 1 --single`*  
*(Explicitly labeled as a single run due to multi-hour execution; no fabricated standard deviation).*

| Workload | Access Pattern | Baseline Tput (Kops/s) | CASCADE Tput (Kops/s) | Tput Diff | Baseline WAF | CASCADE WAF | WAF Reduction | AHLC Switches |
|:---|:---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| **A (50R/50U)** | 50% Read / 50% Update | 61.0 | **79.5** | **+30.3%** | 82.65 | **42.79** | **-48.2%** | 226 |
| **B (95R/5U)** | 95% Read / 5% Update | 563.1 | 521.5 | -7.4% | 53.33 | **32.23** | **-39.6%** | 145 |
| **C (100R)** | 100% Read | 1119.2 | **1149.8** | **+2.7%** | 58.15 | **26.98** | **-53.6%** | 138 |
| **D (95R/5I)** | 95% Read / 5% Insert | 372.4 | **450.0** | **+20.8%** | 55.84 | **26.22** | **-53.0%** | 153 |
| **E (95Scan/5I)** | 95% Scan / 5% Insert | 31.5 | **39.5** | **+25.4%** | 81.31 | **42.38** | **-47.9%** | 235 |
| **F (50R/50RMW)**| 50% Read / 50% RMW | 58.8 | **77.0** | **+31.0%** | 82.65 | **42.79** | **-48.2%** | 226 |

---

## 8. Analysis of the Two Key Engineering Anomalies

Honest scientific reporting requires analyzing where CASCADE loses and explaining the precise engineering mechanisms:

### Anomaly 1: CSB+ Tree Regressing Under 16 Concurrently Contending Writers
*Command: `./rigorous_bench --anomaly1` (N = 160,000 ops across 16 threads)*

```
Data Structure        Throughput       Total Lock Wait    Acquisitions    Avg Wait/Lock
ConcurrentCSBTree     472.5 Kops/s     4,857.6 ms         160,000         30.36 µs
SkipListMemtable      595.0 Kops/s     3,842.9 ms         160,000         24.02 µs
```

**Mechanism**: The Concurrent CSB+ Tree aligns keys contiguously in 64-byte node groups to maximize CPU L1/L2 cache locality during binary searches. However, during node splits, the writer thread must allocate new contiguous child arrays (`allocGroup`) and perform memory copies while holding the exclusive lock (`rw_mu_`). In contrast, a SkipList merely swings forward pointers without contiguous reallocations. Under 16 concurrent writer threads, this structural overhead increases average lock acquisition wait time by **26.4%** (30.36 µs vs 24.02 µs), allowing the SkipList to achieve higher raw concurrent ingestion throughput.

---

### Anomaly 2: Workload F (Read-Modify-Write) Strategy Thrashing
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

**Mechanism**: In Workload F (50% Read, 50% RMW), user transactions continuously alternate between read probes and atomic write updates. The EWMA write velocity tracker experiences rapid oscillations around the high-rate threshold. This causes AHLC to thrash between `HYBRID` and `TIERING` (6 strategy switches in under 65 ms). When switching from Tiering back to Leveling, AHLC triggers cascading merges of accumulated runs, rewriting keys repeatedly compared to the steady, monotonic merge progression of fixed leveling.

---

## 9. Research Gaps & Publication Readiness

### ✅ Resolved Gaps
1. **Real File-Backed Disk I/O (Formerly Gap 1) — RESOLVED**: All SSTables are now stored as real POSIX binary files with 4KB block packing, packed index blocks, serialized Bloom filters, and 32-byte footers.
2. **Persistent WAL & Recovery (Formerly Gap 5) — RESOLVED**: Persistent append-only disk log (`wal.log`) with 64-write group commit (`fsync()`) and automated startup recovery verified in test suite (`testWALRecovery`).
3. **Statistical Rigor (Formerly Gap 8) — RESOLVED**: Fixed repeat schedule (3 repeats each for 100K, 500K, and 1M; single run for 5M) with sample standard deviation and Mann-Whitney U / Welch t-test p-values.
4. **Bloom Filter Saturation (Formerly Gap 2) — RESOLVED**: Monkey-optimal per-key sizing (10–14 bits/key) maintaining FPR < 0.4% at million-key scales.

### ⚠️ Remaining Research Gaps
1. **Production System Comparison**: Compare against RocksDB using `db_bench` under identical hardware and memory limits.
2. **Fine-Grained Partitioning for CSB+ Tree**: Replace the global `rw_mu_` with sub-tree range locks to eliminate contention at 16+ threads.
3. **RMW-Aware Hysteresis**: Suppress rapid AHLC strategy switches when high Gini read skew is detected concurrently with RMW updates.

---

## 10. File Map

```
cascade-research/
├── include/                   ← All header files (public API + inline implementations)
│   ├── common.h               ← Key, Value, KVPair, Config, SpinLock, hash64, TOMBSTONE
│   ├── csb_tree.h             ← ConcurrentCSBTree: CSB+ tree with OLC + epoch-GC + profiling
│   ├── skiplist_mt.h          ← SkipListMemtable: shared_mutex skip list + lock profiling
│   ├── bloom.h                ← BlockedBloomFilter + SStableAccessTracker + BloomAllocator
│   ├── ahlc.h                 ← AHLCEngine FSM + WriteVelocityTracker + Switch Logger
│   ├── lsm.h                  ← LSMEngine: main orchestrator (public API)
│   ├── wal.h                  ← Persistent WAL: append-only file with group commit fsync
│   ├── cache.h                ← LRUCache<K,V> + BlockCache (64MB default)
│   ├── epoch.h                ← EpochManager singleton + EpochGuard RAII (Silo EBMM)
│   ├── metrics.h              ← EngineMetrics: WAF/RAF/SAF counters + LatencyHistogram
│   ├── sstable.h              ← SSTable file layout (4KB blocks, IndexEntry, Bloom, Footer)
│   └── workload.h             ← ZipfianGenerator + YCSB workloads A–F + Op generators
│
├── src/                       ← C++ implementation files
│   ├── ahlc.cpp               ← SSTable compaction overloads (Leveling, Tiering, SubRange)
│   ├── bloom.cpp              ← BlockedBloomFilter serialization + BloomAllocator
│   ├── cache.cpp              ← BlockCache LRU implementation
│   ├── csb_tree.cpp           ← CSB+ tree: insert, search, scan, flush (OLC + epoch-GC)
│   ├── lsm.cpp                ← Full LSM engine: file persistence, WAL recovery, search/scan
│   └── sstable.cpp            ← Real file I/O, block packing, pread(), IndexEntry, BlockCache
│
├── bench/
│   ├── rigorous_bench.cpp     ← Statistically rigorous scale sequence (100K->500K->1M->5M)
│   ├── ycsb_bench.cpp         ← YCSB runner
│   ├── scale_bench.cpp        ← Scale comparison runner
│   ├── smoke_test.cpp         ← Step 3 stability and FD leak test
│   └── results/               ← Raw benchmark logs and markdown summaries
│
├── tests/
│   └── test_all.cpp           ← 7 test suites, 234 assertions (all passing, ASan/TSan clean)
│
├── Makefile                   ← Targets: test · bench · scale · rigorous · asan · tsan · clean
├── README.md                  ← Quick start, benchmark summary, and honest claims
└── CASCADE_RESEARCH.md        ← THIS FILE: comprehensive research guide
```

---

## 11. How to Build and Run

```bash
cd cascade-research

# 1. Correctness tests (234 tests, 0 failed, ~5 seconds)
make test

# 2. AddressSanitizer + UndefinedBehaviorSanitizer (clean)
make asan

# 3. ThreadSanitizer (race-free)
make tsan

# 4. Rigorous Scale Sequence (saves to bench/results/)
./rigorous_bench --scale 100000 --repeats 3 --single
./rigorous_bench --scale 500000 --repeats 3 --single
./rigorous_bench --scale 1000000 --repeats 3 --single
./rigorous_bench --scale 5000000 --repeats 1 --single

# 5. Anomaly Profiling
./rigorous_bench --anomaly1
./rigorous_bench --anomaly2
```

---

*Generated from verified, reproducible runs on Apple Silicon arm64, C++17 `-O2`.*
