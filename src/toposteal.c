#define _GNU_SOURCE
#include "../include/toposteal.h"
#include "../include/topo.h"
#include "../include/pmu.h"
#include "../include/weights.h"
#include "../include/feedback.h"
#include "../include/deque.h"
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#ifdef __linux__
#include <sched.h>
#endif
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

/* --- preemptive/predictive stealing tuning ---
 * Both are deliberately simple constants for v1 (no ML - see the plan's
 * "cheap analytical estimate" choice, matching A2WS over Chung et al.).
 * They become the natural knobs for the required ablation later. */

/* EMA smoothing factor for each worker's rolling task-duration estimate.
 * Higher = adapts faster to recent tasks, lower = smoother/more stable. */
#define DURATION_EMA_ALPHA 0.2

/* Proactively steal when the predicted time to drain our own queue falls
 * below this many average-task-durations of buffer. 2.0 means "steal
 * ahead once we're down to ~1 task of local buffer left", generalizing
 * A2WS's preemptive theft ("start stealing right after the first task
 * finishes") into a duration-aware threshold instead of a fixed count. */
#define STEAL_AHEAD_LOOKAHEAD 2.0

/* ponytail: fixed-capacity ring for latency percentiles - samples past
 * this many are dropped (fine for a percentile *estimate*, not a log).
 * 1<<20 = 8MB, negligible next to deques[]. Grow if a run needs more. */
#define TOPOSTEAL_MAX_LATENCY_SAMPLES (1 << 20)

typedef struct {
    int worker_id;
    struct toposteal_t *ts;
    /* EMA of how long this worker's own tasks take to execute, in
     * nanoseconds. 0.0 = not yet known. Only this worker ever writes it,
     * but other workers now read it cross-thread too (step 3: estimating
     * a victim's "richness" for urgency-aware victim selection), so it's
     * atomic - a relaxed heuristic read/write, like deque_size() and
     * pmu_t's miss_rates[], not a correctness-critical path. */
    _Atomic double avg_duration_ns;
} worker_ctx_t;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

struct toposteal_t {
    topo_t topo;
    pmu_t pmu;
    weights_t weights;
    feedback_t feedback;
    deque_t deques[TOPO_MAX_CORES];
    pthread_t threads[TOPO_MAX_CORES];
    worker_ctx_t contexts[TOPO_MAX_CORES];
    int num_workers;
    int policy;
    _Atomic int keep_running;
    _Atomic int tasks_pending;
    int pmu_ok;
    uint64_t *latency_ns;          /* malloc'd, TOPOSTEAL_MAX_LATENCY_SAMPLES entries */
    _Atomic size_t latency_count;  /* may exceed capacity; clamp on read */
};

static void record_latency(toposteal_t *ts, uint64_t sample_ns) {
    size_t i = atomic_fetch_add_explicit(&ts->latency_count, 1, memory_order_relaxed);
    if (i < TOPOSTEAL_MAX_LATENCY_SAMPLES)
        ts->latency_ns[i] = sample_ns;
}

static int pick_random_victim(int num_workers, int self, unsigned int *seed) {
    if (num_workers <= 1) return -1;
    int v;
    do { v = (int)(rand_r(seed) % (unsigned)num_workers); } while (v == self);
    return v;
}

static void feedback_callback(void *ctx) {
    feedback_t *f = (feedback_t *)ctx;
    feedback_update(f);
}

/* Predicted "richness" of each worker's deque, in nanoseconds of ready
 * work (queue_depth * avg_task_duration) - fed to weights_pick_victim_urgent
 * so victim selection favors workers likely to actually have fast, ready
 * work, not just topologically close ones. Cross-thread reads of
 * avg_duration_ns and deque_size() are both intentionally-relaxed
 * heuristic snapshots (see their own comments) - stale data here only
 * affects steal-target quality, never task-delivery correctness. */
static void build_richness(toposteal_t *ts, double *out) {
    for (int j = 0; j < ts->num_workers; j++) {
        double avg = atomic_load_explicit(&ts->contexts[j].avg_duration_ns, memory_order_relaxed);
        out[j] = (double)deque_size(&ts->deques[j]) * avg;
    }
}

/* Execute a task, update this worker's rolling task-duration estimate, and
 * - if our own queue's predicted drain time is running low - proactively
 * steal one extra task ahead of time and run it immediately, instead of
 * waiting for a reactive empty-deque check next loop iteration. This is
 * the preemptive-stealing extension: workers anticipate starvation instead
 * of only reacting to it once idle.
 *
 * Deliberately NOT implemented as "steal, then deque_push onto our own
 * deque for later": deque_push is single-producer (only the deque's owner
 * thread may call it), and toposteal_submit already pushes onto worker
 * deques from an external (non-owner) thread at startup. Adding a second
 * concurrent pusher here would race with that on `bottom` and can corrupt
 * the index (observed as tasks_pending hanging forever - a swallowed
 * task). Running the extra task immediately avoids any new deque_push
 * call entirely, so no new race is introduced. */
static void run_task_and_steal_ahead(toposteal_t *ts, int id, task_t *task, unsigned int *seed) {
    worker_ctx_t *ctx = &ts->contexts[id];

    uint64_t start = now_ns();
    task->fn(task->arg);
    uint64_t elapsed_ns = now_ns() - start;
    if (task->enqueue_ns != 0) record_latency(ts, now_ns() - task->enqueue_ns);

    /* Exponential moving average - cheap, no history buffer needed */
    double avg = atomic_load_explicit(&ctx->avg_duration_ns, memory_order_relaxed);
    avg = (avg == 0.0) ? (double)elapsed_ns
                        : DURATION_EMA_ALPHA * (double)elapsed_ns + (1.0 - DURATION_EMA_ALPHA) * avg;
    atomic_store_explicit(&ctx->avg_duration_ns, avg, memory_order_relaxed);

    /* TOPOLOGY_REACTIVE = original TopoSteal: no anticipation, only the
     * reactive empty-deque steal in worker_thread below. */
    if (ts->policy == TOPOSTEAL_POLICY_TOPOLOGY_REACTIVE) return;

    /* Predict how much longer our own queue keeps us busy, and steal ahead
     * if that buffer is running low. Skipped until avg_duration_ns is
     * known (first task ever run by this worker) - no point predicting
     * off zero knowledge. */
    if (avg <= 0.0) return;

    size_t own_size = deque_size(&ts->deques[id]);
    double predicted_drain_ns = (double)own_size * avg;
    double buffer_target_ns = STEAL_AHEAD_LOOKAHEAD * avg;

    if (predicted_drain_ns >= buffer_target_ns) return;

    int victim;
    if (ts->policy == TOPOSTEAL_POLICY_PREDICTION_ONLY) {
        victim = pick_random_victim(ts->num_workers, id, seed);
    } else {
        double richness[TOPO_MAX_CORES];
        build_richness(ts, richness);
        victim = weights_pick_victim_urgent(&ts->weights, id, richness, seed);
    }
    if (victim < 0 || victim == id) return;

    task_t stolen;
    if (!deque_steal(&ts->deques[victim], &stolen)) return;

    uint64_t start2 = now_ns();
    stolen.fn(stolen.arg);
    uint64_t elapsed2_ns = now_ns() - start2;
    if (stolen.enqueue_ns != 0) record_latency(ts, now_ns() - stolen.enqueue_ns);
    avg = atomic_load_explicit(&ctx->avg_duration_ns, memory_order_relaxed);
    avg = DURATION_EMA_ALPHA * (double)elapsed2_ns + (1.0 - DURATION_EMA_ALPHA) * avg;
    atomic_store_explicit(&ctx->avg_duration_ns, avg, memory_order_relaxed);
    atomic_fetch_sub(&ts->tasks_pending, 1);
}

static void *worker_thread(void *arg) {
    worker_ctx_t *ctx = (worker_ctx_t *)arg;
    int id = ctx->worker_id;
    toposteal_t *ts = ctx->ts;
    unsigned int seed = (unsigned int)time(NULL) ^ id;
    task_t task;

#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    int phys_cpu = ts->topo.cpu_map[id];
    CPU_SET(phys_cpu, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
#else
    /* pthread_setaffinity_np/cpu_set_t are Linux-only. CPU pinning is a
     * locality *optimization*, not a correctness requirement - stealing
     * and topology-weighted victim selection both still function without
     * it, just without the hard pin. Warn once so it's clear why locality
     * may be looser when running on a non-Linux dev box. */
    static _Atomic int warned = 0;
    if (!atomic_exchange(&warned, 1))
        printf("[toposteal] WARNING: CPU pinning unavailable on this platform, workers unpinned\n");
#endif

    while (atomic_load(&ts->keep_running)) {
        // Step 1: try own deque
        if (deque_pop(&ts->deques[id], &task)) {
            run_task_and_steal_ahead(ts, id, &task, &seed);
            atomic_fetch_sub(&ts->tasks_pending, 1);
            continue;
        }
        // Step 2: pick a victim (reactive fallback - our queue was already empty)
        int victim;
        if (ts->policy == TOPOSTEAL_POLICY_TOPOLOGY_REACTIVE) {
            victim = weights_pick_victim(&ts->weights, id, &seed);
        } else if (ts->policy == TOPOSTEAL_POLICY_PREDICTION_ONLY) {
            victim = pick_random_victim(ts->num_workers, id, &seed);
        } else {
            double richness[TOPO_MAX_CORES];
            build_richness(ts, richness);
            victim = weights_pick_victim_urgent(&ts->weights, id, richness, &seed);
        }
        if (victim < 0 || victim == id) continue;

        // Step 3: steal from victim
        if (deque_steal(&ts->deques[victim], &task)) {
            run_task_and_steal_ahead(ts, id, &task, &seed);
            atomic_fetch_sub(&ts->tasks_pending, 1);
        }
    }
    return NULL;
}

toposteal_t *toposteal_init(int num_workers) {
    return toposteal_init_policy(num_workers, TOPOSTEAL_POLICY_COMBINED);
}

toposteal_t *toposteal_init_policy(int num_workers, int policy) {
    toposteal_t *ts = malloc(sizeof(toposteal_t));
    if (!ts) return NULL;
    memset(ts, 0, sizeof(*ts));

    ts->num_workers = num_workers;
    ts->policy = policy;
    ts->latency_ns = malloc(TOPOSTEAL_MAX_LATENCY_SAMPLES * sizeof(uint64_t));
    atomic_store(&ts->keep_running, 1);

    topo_init(&ts->topo);
    // Clamp topology to the number of workers we actually have
    if (ts->topo.num_cores > num_workers)
        ts->topo.num_cores = num_workers;
    ts->pmu_ok = (pmu_init(&ts->pmu, num_workers, ts->topo.cpu_map) == 0);
    if (!ts->pmu_ok)
        printf("[toposteal] WARNING: PMU unavailable, using static topology weights\n");

    weights_init(&ts->weights, &ts->topo);
    feedback_init(&ts->feedback, &ts->topo, &ts->weights, &ts->pmu);

    for (int i = 0; i < num_workers; i++)
        deque_init(&ts->deques[i]);

    if (ts->pmu_ok) {
        ts->pmu.feedback_cb = feedback_callback;
        ts->pmu.feedback_ctx = &ts->feedback;
        pmu_start(&ts->pmu);
    }

    // Launch worker threads
    for (int i = 0; i < num_workers; i++) {
        ts->contexts[i].worker_id = i;
        ts->contexts[i].ts = ts;
        pthread_create(&ts->threads[i], NULL, worker_thread, &ts->contexts[i]);
    }

    printf("[toposteal] initialised with %d workers\n", num_workers);
    return ts;
}

void toposteal_submit(toposteal_t *ts, void (*fn)(void *), void *arg) {
    // Round-robin submission to worker deques
    static _Atomic int next_worker = 0;
    int target = atomic_fetch_add(&next_worker, 1) % ts->num_workers;
    task_t t = { .fn = fn, .arg = arg, .enqueue_ns = now_ns() };
    atomic_fetch_add(&ts->tasks_pending, 1);
    deque_push(&ts->deques[target], t);
}

void toposteal_wait(toposteal_t *ts) {
    // Spin until all tasks are submitted AND finished executing
    while (atomic_load(&ts->tasks_pending) > 0)
        ;
}

void toposteal_destroy(toposteal_t *ts) {
    atomic_store(&ts->keep_running, 0);
    for (int i = 0; i < ts->num_workers; i++)
        pthread_join(ts->threads[i], NULL);
    if (ts->pmu_ok)
        pmu_stop(&ts->pmu);
    topo_destroy();
    free(ts->latency_ns);
    free(ts);
    printf("[toposteal] shutdown complete\n");
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

void toposteal_print_latency_stats(toposteal_t *ts) {
    size_t n = atomic_load(&ts->latency_count);
    if (n > TOPOSTEAL_MAX_LATENCY_SAMPLES) n = TOPOSTEAL_MAX_LATENCY_SAMPLES;
    if (n == 0) { printf("[toposteal] no latency samples recorded\n"); return; }

    uint64_t *sorted = malloc(n * sizeof(uint64_t));
    memcpy(sorted, ts->latency_ns, n * sizeof(uint64_t));
    qsort(sorted, n, sizeof(uint64_t), cmp_u64);

    double to_ms = 1.0 / 1e6;
    printf("[toposteal] latency (n=%zu): p50=%.4fms p99=%.4fms p99.9=%.4fms max=%.4fms\n",
        n,
        sorted[n * 50 / 100] * to_ms,
        sorted[n * 99 / 100] * to_ms,
        sorted[n - 1 - n / 1000] * to_ms,
        sorted[n - 1] * to_ms);
    free(sorted);
}