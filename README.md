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

### Performance

Pointer-chase benchmark, 4 parallel workers, 5 trials (mean):

| Machine | Topology | Uniform | TopoSteal | Notes |
|---|---|---|---|---|
| 4-core Xeon (single socket, shared L3) | All distances equal (4) | 0.4496s | 0.4806s | No topology heterogeneity to exploit. ~7% overhead from PMU sampler thread. |

TopoSteal is designed for machines with **heterogeneous cache distances**:

- **Multi-cluster CPUs** (Apple M-series: 3 separate L2 caches, distances 1 vs 8) -- topology correctly detected, pending PMU validation
- **Multi-socket NUMA** (cross-socket penalty up to 10x) -- target environment, scheduled for UIUC/NCSA Delta

On uniform topologies (single socket, shared L3), all distances are equal and topology-weighted stealing degenerates to uniform stealing. The measured overhead comes from the PMU background thread, which will be offset by the topology benefit on heterogeneous hardware.

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
bench/         Benchmarks (pointer chase)
examples/      Usage examples (parallel_sum)
```

## How It Works

1. At init, `hwloc` walks the CPU tree and builds a distance matrix between every core pair
2. Distances are converted to steal probabilities (inverse distance, normalized to CDF)
3. Workers pop from their own deque first, then steal from a weighted-random victim
4. A background PMU thread polls cache miss rates every 10ms
5. The feedback loop multiplies base distances by `(1 + normalized_miss_rate)`, penalizing victims with high miss rates
6. Steal probabilities are rebuilt, shifting work toward cache-local, low-contention neighbours
