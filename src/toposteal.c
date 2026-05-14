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
#include <sched.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

typedef struct {
    int worker_id;
    struct toposteal_t *ts;
} worker_ctx_t;

struct toposteal_t {
    topo_t topo;
    pmu_t pmu;
    weights_t weights;
    feedback_t feedback;
    deque_t deques[TOPO_MAX_CORES];
    pthread_t threads[TOPO_MAX_CORES];
    worker_ctx_t contexts[TOPO_MAX_CORES];
    int num_workers;
    _Atomic int keep_running;
    _Atomic int tasks_pending;
};

static void *worker_thread(void *arg) {
    worker_ctx_t *ctx = (worker_ctx_t *)arg;
    int id = ctx->worker_id;
    toposteal_t *ts = ctx->ts;
    unsigned int seed = (unsigned int)time(NULL) ^ id;
    task_t task;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    while (atomic_load(&ts->keep_running)) {
        // Step 1: try own deque
        if (deque_pop(&ts->deques[id], &task)) {
            task.fn(task.arg);
            atomic_fetch_sub(&ts->tasks_pending, 1);
            continue;
        }
        // Step 2: pick a victim
        int victim = weights_pick_victim(&ts->weights, id, &seed);
        if (victim < 0 || victim == id) continue;

        // Step 3: steal from victim
        if (deque_steal(&ts->deques[victim], &task)) {
            task.fn(task.arg);
            atomic_fetch_sub(&ts->tasks_pending, 1);
        }
    }
    return NULL;
}

toposteal_t *toposteal_init(int num_workers) {
    toposteal_t *ts = malloc(sizeof(toposteal_t));
    if (!ts) return NULL;
    memset(ts, 0, sizeof(*ts));

    ts->num_workers = num_workers;
    atomic_store(&ts->keep_running, 1);

    topo_init(&ts->topo);
    // Clamp topology to the number of workers we actually have
    if (ts->topo.num_cores > num_workers)
        ts->topo.num_cores = num_workers;
    if (pmu_init(&ts->pmu, num_workers) == -1) {
        printf("[toposteal] WARNING: PMU init failed, running without PMU\n");
    }
    weights_init(&ts->weights, &ts->topo);
    feedback_init(&ts->feedback, &ts->topo, &ts->weights, &ts->pmu);

    for (int i = 0; i < num_workers; i++)
        deque_init(&ts->deques[i]);

    pmu_start(&ts->pmu);

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
    task_t t = { .fn = fn, .arg = arg };
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
    pmu_stop(&ts->pmu);
    topo_destroy();
    free(ts);
    printf("[toposteal] shutdown complete\n");
}