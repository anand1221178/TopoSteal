#ifndef DEQUE_H
#define DEQUE_H

/* Fixed ring-buffer capacity per deque. deque_push has no overflow
 * detection or backpressure - it unconditionally overwrites slot
 * `bottom % DEQUE_MAX_TASKS`, so if more than DEQUE_MAX_TASKS tasks are
 * ever in flight (submitted but not yet popped/stolen) on one deque, it
 * silently corrupts not-yet-consumed tasks. Found via the DLRM benchmark
 * (bench/dlrm_embedding.c): 20000 tasks round-robined across 4 workers
 * gives ~5000 in flight per deque, wrapping the old 1024-slot buffer
 * ~4.9x and producing wrong (not just slow) output. Raised to 8192 for
 * headroom on Phase-1-scale runs (toposteal_t statically sizes
 * TOPO_MAX_CORES=128 of these, so this isn't free - ~32MB total at 8192,
 * vs ~4MB at 1024 - hence a modest bump, not an unbounded one). Genuine
 * unbounded workloads still need either backpressure in
 * toposteal_submit() or a dynamically-growable deque - neither exists
 * yet; this only raises the ceiling, it doesn't remove it. */
#define DEQUE_MAX_TASKS 8192

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

/* Task struct */
typedef struct
{
    void (*fn)(void *);
    void *arg;

    /* --- proactive/predictive stealing extension ---
     * Both fields are additive and optional: existing designated-initializer
     * call sites (`{ .fn = ..., .arg = ... }`) zero-init them automatically,
     * so this is a no-op for anyone not using the new mechanism yet.
     */

    /* CLOCK_MONOTONIC timestamp (ns) at push time, 0 if unset. Lets a worker
     * measure actual queueing/tail latency, and lets the executing worker
     * time this task's real duration to feed its deque's rolling-average
     * predictor (see queue_stats_t in toposteal.h). */
    uint64_t enqueue_ns;

    /* Optional caller-supplied duration estimate for this task, in
     * nanoseconds. 0 means "unknown" - the runtime falls back to the
     * executing deque's own rolling-average estimate instead. */
    double predicted_ns;
}task_t;

/* Deque strcuture */
typedef struct
{
    _Atomic size_t top;
    _Atomic size_t bottom;
    task_t tasks[DEQUE_MAX_TASKS]; /* We are keeping it constant since the number of workers is constant */

    /* Guards bottom-side access (push and pop). Classic Chase-Lev assumes
     * a single owner thread does push+pop sequentially and only steal (at
     * top) is concurrent - but toposteal_submit() pushes onto a worker's
     * deque from an external (non-owner) thread, which is a second
     * concurrent writer to `bottom` and can race with that worker's own
     * pop, silently corrupting the index and losing a task (found via
     * repeated stress runs - tasks_pending would hang at 1 forever).
     * This mutex serializes push against pop to close that race; steal
     * remains lock-free/CAS-based against top as before. */
    pthread_mutex_t bottom_lock;
}deque_t;

/* Function declares */
void deque_init(deque_t *q);

void deque_push(deque_t *q, task_t t);

/* Returns 1 if success, else 0 */
int deque_pop(deque_t *q, task_t *t_out);

/* Returns 1 if success, else 0 */
int deque_steal(deque_t *q, task_t *t_out);

/* Approximate current depth (bottom - top). This is a heuristic snapshot
 * for scheduling decisions (e.g. predicting when a queue will run dry) -
 * it can race with concurrent push/pop/steal like top/bottom themselves
 * already do, so it is not meant to be exact, only a cheap estimate. */
size_t deque_size(deque_t *q);

#endif