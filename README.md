# TopoSteal

Topology-aware work-stealing runtime that reduces cache misses using live PMU feedback.

Most parallel runtimes (TBB, Cilk, OpenMP) treat all cores as equally close. They're not. TopoSteal reads the CPU's physical topology via `hwloc`, builds a distance matrix, and biases work-stealing toward cache-local neighbours. A background thread polls hardware performance counters (`perf_event_open`) and dynamically adjusts steal probabilities as the workload shifts.

## Architecture

```
+---------------------------------------------------+
|  Application Layer                                 |
|  toposteal_init / submit / wait / destroy          |
+---------------------------------------------------+
|  Topology Graph (hwloc)                            |
|  L2-shared=1, L3-shared=4, same-package=8,        |
|  cross-NUMA=10                                     |
+---------------------------------------------------+
|  Work-Stealing Deques (Chase-Lev, lock-free)       |
|  One per worker, C11 atomics                       |
+---------------------------------------------------+
|  PMU Sampler (background thread, 10ms poll)        |
|  perf_event_open per worker, rolling miss rate     |
+---------------------------------------------------+
|  Feedback Loop                                     |
|  effective_weight = base_dist * (1 + miss_rate)    |
|  Rebuilds steal probability table dynamically      |
+---------------------------------------------------+
```

## Build

```bash
make
```

Produces `libtoposteal.a`. Requires `libhwloc-dev` and `pthreads`.

## Usage

```c
#include "toposteal.h"

toposteal_t *ts = toposteal_init(4);

for (int i = 0; i < 1000; i++)
    toposteal_submit(ts, my_function, &my_args[i]);

toposteal_wait(ts);
toposteal_destroy(ts);
```

## Run Tests

```bash
make test
```

## Benchmark

```bash
make bench_pointer_chase
sudo ./bench_pointer_chase
```

### Correctness

Stress test: 4 workers, 10 seconds continuous push/pop/steal, 81M+ tasks, zero tasks lost.

### Performance (Dual-Socket NUMA)

Benchmarked on a dual-socket Intel Xeon E5-2690v3 (24 cores, NUMA distances 10/21) from the CHPC Lengau cluster. Pointer-chase benchmark with 64 MB per-NUMA arrays exceeding L3 cache (30 MB), asymmetric task placement to force cross-socket stealing. Three scheduling modes compared: Uniform, TopoStatic (1/d^2 weights), and Topo+PMU (dynamic feedback). 10 trials per configuration.

| Configuration | Uniform (s) | TopoStatic (s) | Topo+PMU (s) | Speedup (Static / PMU) |
|---|---|---|---|---|
| Heavy imbal., long (1200 tasks, 2M iters) | 8.555 | 7.325 | 7.322 | **1.17x / 1.17x** |
| Heavy imbal., med (1200 tasks, 1M iters) | 4.285 | 3.663 | 3.651 | **1.17x / 1.17x** |
| Moderate imbal. (1200 tasks, 2M iters, 4 prod) | 8.325 | 7.224 | 7.216 | **1.15x / 1.15x** |
| Many short tasks (2400 tasks, 500K iters) | 4.286 | 3.664 | 3.647 | **1.17x / 1.18x** |
| Extreme imbal. (1200 tasks, 1M iters, 1 prod) | 4.372 | 3.764 | 3.746 | **1.16x / 1.17x** |

**Steal locality:** Uniform ~50% same-socket steals. Both topology-aware modes achieve **~85%** same-socket steals.

**PMU feedback:** The dynamic PMU mode matches or slightly outperforms static topology weights. On this homogeneous workload, the PMU has limited asymmetry to exploit. Larger gains are expected on heterogeneous workloads and 4+ socket systems.

## API

| Function | Description |
|---|---|
| `toposteal_init(int n)` | Create runtime with `n` worker threads |
| `toposteal_submit(ts, fn, arg)` | Submit a task for parallel execution |
| `toposteal_wait(ts)` | Block until all submitted tasks complete |
| `toposteal_destroy(ts)` | Shut down workers and free resources |

## Project Structure

```
include/       Headers (topo.h, pmu.h, deque.h, weights.h, feedback.h, toposteal.h)
src/           Implementation
tests/         Unit tests and stress tests
bench/         Benchmarks, results CSV, and plotting scripts
examples/      Usage examples (parallel_sum)
paper/         IEEE conference paper (LaTeX)
scripts/       Cluster job scripts (PBS)
```

## How It Works

1. At init, `hwloc` walks the CPU tree and builds a distance matrix between every core pair
2. Distances are converted to steal probabilities using inverse-square weighting (w = 1/d^2), normalized to a CDF
3. Workers pop from their own deque first, then steal from a weighted-random victim
4. A background PMU thread polls cache miss rates every 10ms via `perf_event_open`
5. The feedback loop multiplies base distances by `(1 + normalized_miss_rate)`, penalizing victims with high miss rates, and rebuilds steal probabilities every 50ms

The benchmark evaluates all three modes: uniform (baseline), topology-static (steps 1-3), and topology+PMU (steps 1-5).
