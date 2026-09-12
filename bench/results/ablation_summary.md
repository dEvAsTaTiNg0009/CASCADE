# CASCADE Architectural Ablation Study (8 Configurations)

> Evaluates {SkipList / CSB+} × {Fixed Leveling / AHLC} × {Uniform / Adaptive Bloom} on Workload A (50% Read / 50% Update).

### 1. Ablation Study: 200,000 Operations (200K Scale)

| MemTable | Compaction | Bloom Sizing | Throughput (Kops/s) | WAF | RAF | SAF | Bloom FPR (%) | AHLC Switches |
|---|---|---|:---:|:---:|:---:|:---:|:---:|:---:|
| SkipList | Fixed Leveling | Uniform | 130.3 | 67.47 | 0.33 | 10.66 | 0.1030% | 0 |
| SkipList | Fixed Leveling | Adaptive (Dual-Trigger) | 78.2 | 167.56 | 0.37 | 10.91 | 0.1146% | 0 |
| SkipList | AHLC | Uniform | 96.5 | 147.22 | 0.44 | 10.84 | 0.1036% | 9 |
| SkipList | AHLC | Adaptive (Dual-Trigger) | 88.1 | 91.79 | 0.45 | 11.51 | 0.1086% | 9 |
| CSB+ Tree | Fixed Leveling | Uniform | 70.5 | 233.48 | 0.37 | 11.37 | 0.1193% | 0 |
| CSB+ Tree | Fixed Leveling | Adaptive (Dual-Trigger) | 92.8 | 191.83 | 0.37 | 10.72 | 0.1064% | 0 |
| CSB+ Tree | AHLC | Uniform | 110.6 | 7.32 | 0.45 | 11.29 | 0.0792% | 9 |
| CSB+ Tree | AHLC | Adaptive (Dual-Trigger) *(CASCADE)* | 95.7 | 7.62 | 0.45 | 11.37 | 0.1029% | 9 |

### 2. Ablation Study: 1,000,000 Operations (1M Scale)

| MemTable | Compaction | Bloom Sizing | Throughput (Kops/s) | WAF | RAF | SAF | Bloom FPR (%) | AHLC Switches |
|---|---|---|:---:|:---:|:---:|:---:|:---:|:---:|
| SkipList | Fixed Leveling | Uniform | 91.7 | 27.50 | 0.46 | 2.70 | 0.1123% | 0 |
| SkipList | Fixed Leveling | Adaptive (Dual-Trigger) | 80.4 | 24.55 | 0.50 | 3.03 | 0.1103% | 0 |
| SkipList | AHLC | Uniform | 77.6 | 17.87 | 0.60 | 3.62 | 0.0829% | 44 |
| SkipList | AHLC | Adaptive (Dual-Trigger) | 71.1 | 17.31 | 0.57 | 4.20 | 0.0865% | 49 |
| CSB+ Tree | Fixed Leveling | Uniform | 73.0 | 53.99 | 0.50 | 3.58 | 0.1071% | 0 |
| CSB+ Tree | Fixed Leveling | Adaptive (Dual-Trigger) | 66.2 | 24.44 | 0.50 | 3.35 | 0.1048% | 0 |
| CSB+ Tree | AHLC | Uniform | 71.6 | 17.33 | 0.60 | 3.91 | 0.0847% | 44 |
| CSB+ Tree | AHLC | Adaptive (Dual-Trigger) *(CASCADE)* | 62.3 | 20.66 | 0.57 | 3.96 | 0.0920% | 48 |
