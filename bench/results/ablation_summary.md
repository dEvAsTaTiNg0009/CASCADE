# CASCADE Architectural Ablation Study (8 Configurations)

> Evaluates {SkipList / CSB+} x {Fixed Leveling / AHLC} x {Uniform / Adaptive Bloom} on Workload A (50% Read / 50% Update).
> Each row = 5 repeats (seed=42..46); values are **mean +/- std**.
> "Uniform" uses staticUniformBloomInit(): filters sized once at startup, never re-triggered by AHLC or access frequency.
> "Adaptive" uses dual-trigger reallocation (structural + frequency).
> Raw per-run data: bench/results/ablation_200K_allruns.csv and bench/results/ablation_1M_allruns.csv

### 1. Ablation Study: 200,000 Operations (200K Scale)

| MemTable | Compaction | Bloom Sizing | Throughput (mean +/- std Kops/s) | WAF (mean +/- std) | RAF | SAF | Bloom FPR (%) | AHLC Switches |
|---|---|---|:---:|:---:|:---:|:---:|:---:|:---:|
| SkipList | Fixed Leveling | Uniform (Static) | 572.5 +/- 97.5 | 15.32 +/- 16.45 | 0.55 +/- 0.02 | 17.65 +/- 0.59 | 2.5512% +/- 1.1587% | 0.0 |
| SkipList | Fixed Leveling | Adaptive (Dual-Trigger) | 101.2 +/- 4.1 | 13.29 +/- 11.38 | 2.13 +/- 0.21 | 18.60 +/- 0.45 | 50.1165% +/- 3.3006% | 0.0 |
| SkipList | AHLC | Uniform (Static) | 738.8 +/- 71.6 | 7.73 +/- 2.10 | 0.62 +/- 0.02 | 19.70 +/- 0.47 | 1.1742% +/- 0.2722% | 9.0 |
| SkipList | AHLC | Adaptive (Dual-Trigger) | 93.6 +/- 2.3 | 9.31 +/- 3.61 | 2.81 +/- 0.24 | 19.91 +/- 0.91 | 45.6371% +/- 1.2588% | 9.0 |
| CSB+ Tree | Fixed Leveling | Uniform (Static) | 733.9 +/- 115.0 | 8.45 +/- 0.85 | 0.58 +/- 0.01 | 19.58 +/- 0.41 | 2.2769% +/- 0.5992% | 0.0 |
| CSB+ Tree | Fixed Leveling | Adaptive (Dual-Trigger) | 90.5 +/- 2.4 | 8.37 +/- 2.46 | 2.11 +/- 0.19 | 20.72 +/- 0.56 | 43.4835% +/- 2.3889% | 0.0 |
| CSB+ Tree | AHLC | Uniform (Static) | 754.6 +/- 155.8 | 8.87 +/- 3.40 | 0.62 +/- 0.02 | 20.98 +/- 1.46 | 1.0192% +/- 0.2322% | 9.0 |
| CSB+ Tree | AHLC | Adaptive (Dual-Trigger) *(CASCADE)* | 95.5 +/- 1.9 | 8.26 +/- 2.42 | 2.82 +/- 0.22 | 19.87 +/- 0.44 | 46.0279% +/- 1.3120% | 9.0 |

### 2. Ablation Study: 1,000,000 Operations (1M Scale)

| MemTable | Compaction | Bloom Sizing | Throughput (mean +/- std Kops/s) | WAF (mean +/- std) | RAF | SAF | Bloom FPR (%) | AHLC Switches |
|---|---|---|:---:|:---:|:---:|:---:|:---:|:---:|
| SkipList | Fixed Leveling | Uniform (Static) | 550.2 +/- 149.1 | 26.34 +/- 9.85 | 0.57 +/- 0.02 | 4.02 +/- 0.43 | 1.8537% +/- 0.4647% | 0.0 |
| SkipList | Fixed Leveling | Adaptive (Dual-Trigger) | 76.5 +/- 3.5 | 24.11 +/- 7.17 | 2.20 +/- 0.05 | 4.80 +/- 0.43 | 46.8008% +/- 3.7158% | 0.0 |
| SkipList | AHLC | Uniform (Static) | 506.5 +/- 35.8 | 21.79 +/- 3.98 | 0.69 +/- 0.02 | 6.31 +/- 0.54 | 1.0637% +/- 0.3829% | 48.0 |
| SkipList | AHLC | Adaptive (Dual-Trigger) | 17.0 +/- 19.0 | 25.24 +/- 1.00 | 2.46 +/- 0.02 | 7.03 +/- 0.40 | 36.4936% +/- 1.8295% | 49.0 |
| CSB+ Tree | Fixed Leveling | Uniform (Static) | 604.9 +/- 153.4 | 29.19 +/- 12.45 | 0.65 +/- 0.01 | 5.37 +/- 0.58 | 2.3530% +/- 0.4307% | 0.0 |
| CSB+ Tree | Fixed Leveling | Adaptive (Dual-Trigger) | 45.0 +/- 30.0 | 26.00 +/- 7.14 | 2.15 +/- 0.03 | 5.50 +/- 0.30 | 44.4645% +/- 1.3289% | 0.0 |
| CSB+ Tree | AHLC | Uniform (Static) | 554.7 +/- 77.8 | 20.36 +/- 4.13 | 0.68 +/- 0.03 | 6.98 +/- 0.48 | 0.9499% +/- 0.7913% | 48.0 |
| CSB+ Tree | AHLC | Adaptive (Dual-Trigger) *(CASCADE)* | 51.0 +/- 3.3 | 25.42 +/- 1.24 | 2.48 +/- 0.03 | 7.39 +/- 0.52 | 36.6445% +/- 2.0168% | 49.0 |
