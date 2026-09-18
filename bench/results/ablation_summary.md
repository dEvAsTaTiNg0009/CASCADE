# CASCADE Architectural Ablation Study (8 Configurations)

> Evaluates {SkipList / CSB+} x {Fixed Leveling / AHLC} x {Uniform / Adaptive Bloom} on Workload A (50% Read / 50% Update).
> Each row = 5 repeats (seed=42..46); values are **mean +/- std**.
> "Uniform" uses a fixed allocation policy; filter contents are rebuilt when SSTables change.
> "Adaptive" uses dual-trigger reallocation (structural + frequency).
> Raw per-run data: bench/results/ablation_200K_allruns.csv and bench/results/ablation_1M_allruns.csv

### 1. Ablation Study: 200,000 Operations (200K Scale)

| MemTable | Compaction | Bloom Sizing | Throughput (mean +/- std Kops/s) | WAF (mean +/- std) | RAF | SAF | Bloom FPR (%) | AHLC Switches |
|---|---|---|:---:|:---:|:---:|:---:|:---:|:---:|
| SkipList | Fixed Leveling | Uniform (Fixed Allocation) | 38.4 +/- 5.2 | 29.50 +/- 32.45 | 0.56 +/- 0.01 | 25.32 +/- 1.43 | 0.2505% +/- 0.0041% | 0.0 |
| SkipList | Fixed Leveling | Adaptive (Dual-Trigger) | 37.4 +/- 3.0 | 8.60 +/- 2.67 | 2.16 +/- 0.20 | 25.77 +/- 0.50 | 46.4280% +/- 3.0812% | 0.0 |
| SkipList | AHLC | Uniform (Fixed Allocation) | 39.5 +/- 1.1 | 7.61 +/- 3.24 | 0.63 +/- 0.01 | 26.64 +/- 0.96 | 0.2622% +/- 0.0215% | 8.8 |
| SkipList | AHLC | Adaptive (Dual-Trigger) | 38.4 +/- 1.4 | 8.26 +/- 2.42 | 2.91 +/- 0.23 | 26.69 +/- 0.44 | 46.0131% +/- 1.1756% | 9.0 |
| CSB+ Tree | Fixed Leveling | Uniform (Fixed Allocation) | 39.0 +/- 2.3 | 8.70 +/- 3.33 | 0.56 +/- 0.00 | 26.25 +/- 0.87 | 0.2616% +/- 0.0187% | 0.0 |
| CSB+ Tree | Fixed Leveling | Adaptive (Dual-Trigger) | 38.9 +/- 0.4 | 8.38 +/- 2.57 | 2.19 +/- 0.23 | 27.13 +/- 0.75 | 44.2174% +/- 2.2404% | 0.0 |
| CSB+ Tree | AHLC | Uniform (Fixed Allocation) | 36.7 +/- 5.7 | 8.43 +/- 3.25 | 0.63 +/- 0.01 | 27.32 +/- 0.52 | 0.2636% +/- 0.0091% | 9.0 |
| CSB+ Tree | AHLC | Adaptive (Dual-Trigger) *(CASCADE)* | 35.5 +/- 3.9 | 13.36 +/- 6.19 | 2.78 +/- 0.24 | 26.65 +/- 1.68 | 45.0651% +/- 3.0450% | 9.4 |

### 2. Ablation Study: 1,000,000 Operations (1M Scale)

| MemTable | Compaction | Bloom Sizing | Throughput (mean +/- std Kops/s) | WAF (mean +/- std) | RAF | SAF | Bloom FPR (%) | AHLC Switches |
|---|---|---|:---:|:---:|:---:|:---:|:---:|:---:|
| SkipList | Fixed Leveling | Uniform (Fixed Allocation) | 36.0 +/- 6.8 | 27.91 +/- 11.97 | 0.59 +/- 0.00 | 5.28 +/- 0.92 | 0.2541% +/- 0.0088% | 0.0 |
| SkipList | Fixed Leveling | Adaptive (Dual-Trigger) | 37.4 +/- 3.0 | 23.69 +/- 7.92 | 2.21 +/- 0.04 | 5.40 +/- 0.36 | 46.1861% +/- 2.4306% | 0.0 |
| SkipList | AHLC | Uniform (Fixed Allocation) | 28.7 +/- 1.8 | 21.58 +/- 2.43 | 0.66 +/- 0.01 | 7.03 +/- 0.55 | 0.2567% +/- 0.0089% | 47.8 |
| SkipList | AHLC | Adaptive (Dual-Trigger) | 26.2 +/- 1.9 | 25.46 +/- 1.33 | 2.52 +/- 0.03 | 7.48 +/- 0.52 | 37.5920% +/- 2.0727% | 49.0 |
| CSB+ Tree | Fixed Leveling | Uniform (Fixed Allocation) | 33.8 +/- 3.0 | 29.23 +/- 12.51 | 0.59 +/- 0.00 | 5.67 +/- 0.58 | 0.2546% +/- 0.0095% | 0.0 |
| CSB+ Tree | Fixed Leveling | Adaptive (Dual-Trigger) | 33.2 +/- 1.3 | 26.00 +/- 7.14 | 2.20 +/- 0.03 | 5.79 +/- 0.30 | 45.7985% +/- 1.2398% | 0.0 |
| CSB+ Tree | AHLC | Uniform (Fixed Allocation) | 26.8 +/- 2.3 | 20.36 +/- 4.13 | 0.66 +/- 0.01 | 7.28 +/- 0.48 | 0.2535% +/- 0.0098% | 48.0 |
| CSB+ Tree | AHLC | Adaptive (Dual-Trigger) *(CASCADE)* | 25.3 +/- 1.9 | 25.42 +/- 1.24 | 2.54 +/- 0.03 | 7.69 +/- 0.52 | 38.0659% +/- 2.0355% | 49.0 |
