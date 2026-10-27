# Settlement Engine

A liquidity-aware settlement engine for interbank payments, written in C++20.

Large-value payment systems settle billions in transfers every day, and each payment needs the payer to have the money at that moment. When banks are waiting on each other's incoming payments, settlement stalls even though everyone could pay if the payments went through together. This project implements the algorithms that resolve that problem, from exact optimisation to a real-time engine that settles payments as they arrive.

## Highlights

- **83% vs 75% of value settled with the same liquidity** on a stressed payment day, by resolving gridlock
  with a max-flow based solver instead of settling payments one at a time; slowest-1% delays fall by 38%.
- **Real-time engine** processing about 70,000 payments per second, with queue recycling, configurable
  batch policies and deadlines, fully deterministic and replayable.
- **Exact and heuristic solvers** for multilateral netting, gridlock resolution and fee-aware routing:
  bitmask DP, meet-in-the-middle, branch and bound, Dinic max flow, min-cost flow and an LP upper bound.
- **Verified against independent brute-force oracles** on thousands of random cases per solver, with
  integer-only money arithmetic and sanitizer builds.

## Results

All results are deterministic and can be reproduced with the commands shown (run from `build/`).

**Real-time settlement under stress.** 100 banks, 50,000 payments, each bank starting with 1% of its daily
outflow, most payments answered by a payment in the opposite direction
([`configs/stressed_day.cfg`](configs/stressed_day.cfg)).

| Policy | Value settled | p99 delay |
|---|---:|---:|
| Settle on arrival, retry queues on incoming funds | 75.5% | 4,489 |
| + periodic batches, FIFO | 74.7% | 4,384 |
| + periodic batches, drop-violators | 78.8% | 4,082 |
| + periodic batches, max-flow relaxation | **83.4%** | **2,769** |

```
./gen_day --config ../configs/stressed_day.cfg --out day.csv
./simulate --day day.csv --sim ../configs/sim/recycle_only.cfg --sim ../configs/sim/fifo_300.cfg \
           --sim ../configs/sim/drop_300.cfg --sim ../configs/sim/relax_300.cfg
```

**Gridlock heuristics against the exact optimum.** 6,720 instances from four generator families (random,
hub-and-spoke, cycles, clusters) with up to 120 payments; the optimum is computed by branch and bound. The
table shows the share of settleable value each heuristic leaves behind.

| Bank liquidity | FIFO | Drop-violators | Max-flow relaxation |
|---:|---:|---:|---:|
| 10% | 83.8% | 69.0% | **62.0%** |
| 20% | 46.2% | 31.3% | **24.4%** |
| 50% | 6.2% | 4.7% | **3.6%** |
| 75% | 1.9% | 1.4% | **1.4%** |

```
./bench_gridlock
```

**Fee-aware routing.** 1,000 banks connected through 20 hubs (6,260 channels), 1,000 payments.

| Strategy | Payments routed | Average fee |
|---|---:|---:|
| Cheapest single path | 903 | 43.9 bps |
| Multi-path min-cost flow | **968** | **38.9 bps** |

```
./test_task14
```

## Quick start

Requires CMake 3.16+ and a C++20 compiler.

```
mkdir build && cd build
cmake .. && make
make test
```

Generate a payment day and compare settlement policies on it:

```
./gen_day --config ../configs/default_day.cfg --out day.csv
./simulate --day day.csv --sim ../configs/sim/recycle_only.cfg --sim ../configs/sim/relax_300.cfg
```

Payment days and policies are plain-text configuration files in [`configs/`](configs). `simulate` can also
write a per-payment event log (`--events`) and a summary CSV (`--csv`).

## Components

| Component | Problem | Methods |
|---|---|---|
| Netting | Settle a set of debts with the fewest transfers | Bitmask subset DP, meet-in-the-middle, pair and triple matching |
| Gridlock resolution | Settle the most value without any bank going below its limit (NP-hard) | Branch and bound, max-flow relaxation, drop-violators, FIFO bypass |
| Bounds | Certify how close a solution is to optimal | Min-cost flow LP relaxation, solved per connected component |
| Routing | Move a payment across fee-charging channels at least cost | Min-cost flow (successive shortest paths with potentials) |
| Real-time engine | Settle payments as they arrive over a business day | Discrete-event simulation with recycling cascades and solver-driven batches |
| Workload generator | Produce realistic payment days | Concentrated bank sizes, heavy-tailed amounts, intraday profiles, reciprocal flows |

## Work in progress

- Tighter LP bound to certify solution quality on large instances
- Liquidity experiments: value settled against available liquidity, liquidity saved by batching
- Plots and a technical write-up