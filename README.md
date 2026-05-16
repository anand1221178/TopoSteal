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

Benchmarked on a dual-socket Intel Xeon E5-2690v3 (24 cores, NUMA distances 10/21) from the CHPC Lengau cluster. Pointer-chase benchmark with 64 MB per-NUMA arrays exceeding L3 cache (30 MB), asymmetric task placement to force cross-socket stealing. 10 trials per configuration.

| Configuration | Uniform (s) | TopoSteal (s) | Speedup |
|---|---|---|---|
| Heavy imbalance, long (1200 tasks, 2M iters) | 8.755 | 7.615 | **1.15x** |
| Heavy imbalance, med (1200 tasks, 1M iters) | 4.379 | 3.714 | **1.18x** |
| Moderate imbalance (1200 tasks, 2M iters, 4 prod) | 8.484 | 7.354 | **1.15x** |
| Many short tasks (2400 tasks, 500K iters) | 4.362 | 3.736 | **1.17x** |
| Extreme imbalance (1200 tasks, 1M iters, 1 prod) | 4.408 | 3.764 | **1.17x** |

**Steal locality:** Uniform stealing achieves ~50% same-socket steals (random on 2 sockets). TopoSteal increases this to **85%**, reducing cross-socket DRAM traffic significantly.

The 15-18% speedup is consistent with the NUMA latency ratio (~1.75x remote vs local) and the fraction of work that is stolen vs self-processed. Larger gains are expected on 4+ socket systems where the random-steal penalty grows quadratically.

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
4. **(Optional, library mode)** A background PMU thread polls cache miss rates every 10ms via `perf_event_open`
5. **(Optional, library mode)** The feedback loop multiplies base distances by `(1 + normalized_miss_rate)`, penalizing victims with high miss rates, and rebuilds steal probabilities dynamically

The benchmark results above use static topology weights only (steps 1-3) to isolate the effect of topology-aware victim selection. The PMU feedback (steps 4-5) is implemented in the library and activated when using the `toposteal_init` API directly.
