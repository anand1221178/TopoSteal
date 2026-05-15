#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include "../include/toposteal.h"
#include "../include/deque.h"

#define NUM_WORKERS    56
#define ARRAY_SIZE     (8 * 1024 * 1024)
#define CHASE_ITERS    500000
#define NUM_TASKS      512
#define TRIALS         5

static size_t *chase_array;

typedef struct {
    int task_id;
    volatile size_t result;
} chase_arg_t;

static void pointer_chase_task(void *arg) {
    chase_arg_t *ca = (chase_arg_t *)arg;
    size_t idx = ca->task_id * 7919;
    for (int i = 0; i < CHASE_ITERS; i++)
        idx = chase_array[idx % ARRAY_SIZE];
    ca->result = idx;
}

static void shuffle_array(size_t *arr, size_t n) {
    unsigned int seed = 42;
    for (size_t i = n - 1; i > 0; i--) {
        size_t j = rand_r(&seed) % (i + 1);
        size_t tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

typedef struct {
    deque_t *queues;
    int id;
    _Atomic int *tasks_done;
} uniform_ctx_t;

static void *uniform_worker(void *arg) {
    uniform_ctx_t *ctx = (uniform_ctx_t *)arg;
    unsigned int seed = (unsigned int)time(NULL) ^ ctx->id;
    task_t task;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(ctx->id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    while (atomic_load(ctx->tasks_done) < NUM_TASKS) {
        if (deque_pop(&ctx->queues[ctx->id], &task)) {
            task.fn(task.arg);
            atomic_fetch_add(ctx->tasks_done, 1);
            continue;
        }
        int victim = rand_r(&seed) % NUM_WORKERS;
        if (victim != ctx->id && deque_steal(&ctx->queues[victim], &task)) {
            task.fn(task.arg);
            atomic_fetch_add(ctx->tasks_done, 1);
        }
    }
    return NULL;
}

static double run_uniform_bench(void) {
    deque_t queues[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; i++)
        deque_init(&queues[i]);

    chase_arg_t args[NUM_TASKS];
    for (int i = 0; i < NUM_TASKS; i++) {
        args[i].task_id = i;
        args[i].result = 0;
        task_t t = { .fn = pointer_chase_task, .arg = &args[i] };
        deque_push(&queues[i % NUM_WORKERS], t);
    }

    _Atomic int tasks_done = 0;
    uniform_ctx_t ctxs[NUM_WORKERS];
    pthread_t threads[NUM_WORKERS];

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < NUM_WORKERS; i++) {
        ctxs[i] = (uniform_ctx_t){ .queues = queues, .id = i, .tasks_done = &tasks_done };
        pthread_create(&threads[i], NULL, uniform_worker, &ctxs[i]);
    }
    for (int i = 0; i < NUM_WORKERS; i++)
        pthread_join(threads[i], NULL);

    clock_gettime(CLOCK_MONOTONIC, &end);
    return (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
}

static double run_toposteal_bench(void) {
    toposteal_t *ts = toposteal_init(NUM_WORKERS);
    chase_arg_t args[NUM_TASKS];

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < NUM_TASKS; i++) {
        args[i].task_id = i;
        args[i].result = 0;
        toposteal_submit(ts, pointer_chase_task, &args[i]);
    }
    toposteal_wait(ts);

    clock_gettime(CLOCK_MONOTONIC, &end);
    toposteal_destroy(ts);

    return (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
}

int main() {
    chase_array = malloc(ARRAY_SIZE * sizeof(size_t));
    for (size_t i = 0; i < ARRAY_SIZE; i++)
        chase_array[i] = i;
    shuffle_array(chase_array, ARRAY_SIZE);

    printf("Pointer-Chase Benchmark\n");
    printf("Array: %zu KB (fits in L2), %d tasks, %d iterations/task, %d trials\n",
        (ARRAY_SIZE * sizeof(size_t)) / 1024,
        NUM_TASKS, CHASE_ITERS, TRIALS);
    printf("Both configurations use %d parallel workers (1 per PU).\n\n", NUM_WORKERS);

    double uniform_total = 0, topo_total = 0;

    for (int t = 0; t < TRIALS; t++) {
        double u = run_uniform_bench();
        double ts = run_toposteal_bench();
        uniform_total += u;
        topo_total += ts;
        printf("Trial %d:  Uniform=%.4fs  TopoSteal=%.4fs  Speedup=%.2fx\n",
            t + 1, u, ts, u / ts);
    }

    double u_avg = uniform_total / TRIALS;
    double t_avg = topo_total / TRIALS;

    printf("\n--- Results (mean of %d trials) ---\n", TRIALS);
    printf("Uniform stealing:    %.4fs\n", u_avg);
    printf("TopoSteal:           %.4fs\n", t_avg);
    printf("Speedup:             %.2fx\n", u_avg / t_avg);

    free(chase_array);
    return 0;
}
