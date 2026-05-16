#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <sys/mman.h>
#include <math.h>
#include "../include/topo.h"
#include "../include/deque.h"
#include "../include/weights.h"

#define NUM_WORKERS    24
#define TRIALS         10

static topo_t topo;
static size_t *numa0_array;
static size_t *numa1_array;

/* Per-worker steal counters */
static _Atomic long local_steals[128];
static _Atomic long remote_steals[128];
static _Atomic long failed_steals[128];
static _Atomic long own_tasks[128];

typedef struct {
    int task_id;
    int use_numa0;
    volatile size_t result;
} chase_arg_t;

static int chase_iters_global;

static void pointer_chase_task(void *arg) {
    chase_arg_t *ca = (chase_arg_t *)arg;
    size_t *arr = ca->use_numa0 ? numa0_array : numa1_array;
    size_t idx = ca->task_id * 7919;
    for (int i = 0; i < chase_iters_global; i++)
        idx = arr[idx % (8 * 1024 * 1024)];
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
    size_t *arr;
    size_t count;
    int cpu;
} touch_arg_t;

static void *touch_thread(void *arg) {
    touch_arg_t *ta = (touch_arg_t *)arg;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(ta->cpu, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    for (size_t i = 0; i < ta->count; i++)
        ta->arr[i] = i;
    return NULL;
}

static int is_same_socket(int a, int b) {
    return topo.distance[a][b] < TOPO_DIST_NUMA;
}

typedef struct {
    deque_t *queues;
    int id;
    int use_topo;
    int num_tasks;
    weights_t *weights;
    _Atomic int *tasks_done;
} worker_ctx_t;

static void *worker_fn(void *arg) {
    worker_ctx_t *ctx = (worker_ctx_t *)arg;
    unsigned int seed = (unsigned int)time(NULL) ^ ctx->id;
    task_t task;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(topo.cpu_map[ctx->id], &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    while (atomic_load(ctx->tasks_done) < ctx->num_tasks) {
        if (deque_pop(&ctx->queues[ctx->id], &task)) {
            task.fn(task.arg);
            atomic_fetch_add(&ctx->tasks_done[0], 1);
            atomic_fetch_add(&own_tasks[ctx->id], 1);
            continue;
        }
        int victim;
        if (ctx->use_topo) {
            victim = weights_pick_victim(ctx->weights, ctx->id, &seed);
        } else {
            victim = rand_r(&seed) % NUM_WORKERS;
        }
        if (victim >= 0 && victim != ctx->id) {
            if (deque_steal(&ctx->queues[victim], &task)) {
                task.fn(task.arg);
                atomic_fetch_add(&ctx->tasks_done[0], 1);
                if (is_same_socket(ctx->id, victim))
                    atomic_fetch_add(&local_steals[ctx->id], 1);
                else
                    atomic_fetch_add(&remote_steals[ctx->id], 1);
            } else {
                atomic_fetch_add(&failed_steals[ctx->id], 1);
            }
        }
    }
    return NULL;
}

typedef struct {
    double time;
    long total_local_steals;
    long total_remote_steals;
    long total_failed_steals;
    long total_own;
} bench_result_t;

static bench_result_t run_bench(int use_topo, weights_t *weights,
                                 chase_arg_t *args, int num_tasks,
                                 int producers_per_node) {
    deque_t queues[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; i++)
        deque_init(&queues[i]);

    /* Reset counters */
    for (int i = 0; i < NUM_WORKERS; i++) {
        atomic_store(&local_steals[i], 0);
        atomic_store(&remote_steals[i], 0);
        atomic_store(&failed_steals[i], 0);
        atomic_store(&own_tasks[i], 0);
    }

    int tasks_per_producer = num_tasks / (2 * producers_per_node);
    int t_idx = 0;

    /* NUMA 0 producers: workers 0 .. producers_per_node-1 */
    for (int w = 0; w < producers_per_node; w++) {
        for (int j = 0; j < tasks_per_producer; j++) {
            args[t_idx].task_id = t_idx;
            args[t_idx].use_numa0 = 1;
            args[t_idx].result = 0;
            task_t t = { .fn = pointer_chase_task, .arg = &args[t_idx] };
            deque_push(&queues[w], t);
            t_idx++;
        }
    }
    /* NUMA 1 producers: workers 12 .. 12+producers_per_node-1 */
    for (int w = 12; w < 12 + producers_per_node; w++) {
        for (int j = 0; j < tasks_per_producer; j++) {
            args[t_idx].task_id = t_idx;
            args[t_idx].use_numa0 = 0;
            args[t_idx].result = 0;
            task_t t = { .fn = pointer_chase_task, .arg = &args[t_idx] };
            deque_push(&queues[w], t);
            t_idx++;
        }
    }

    _Atomic int tasks_done = 0;
    worker_ctx_t ctxs[NUM_WORKERS];
    pthread_t threads[NUM_WORKERS];

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < NUM_WORKERS; i++) {
        ctxs[i] = (worker_ctx_t){
            .queues = queues, .id = i, .use_topo = use_topo,
            .weights = weights, .tasks_done = &tasks_done,
            .num_tasks = num_tasks
        };
        pthread_create(&threads[i], NULL, worker_fn, &ctxs[i]);
    }
    for (int i = 0; i < NUM_WORKERS; i++)
        pthread_join(threads[i], NULL);

    clock_gettime(CLOCK_MONOTONIC, &end);

    bench_result_t r;
    r.time = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    r.total_local_steals = 0;
    r.total_remote_steals = 0;
    r.total_failed_steals = 0;
    r.total_own = 0;
    for (int i = 0; i < NUM_WORKERS; i++) {
        r.total_local_steals += atomic_load(&local_steals[i]);
        r.total_remote_steals += atomic_load(&remote_steals[i]);
        r.total_failed_steals += atomic_load(&failed_steals[i]);
        r.total_own += atomic_load(&own_tasks[i]);
    }
    return r;
}

static void run_config(const char *label, int num_tasks, int chase_iters,
                       int producers_per_node, weights_t *weights, FILE *csv) {
    chase_iters_global = chase_iters;
    chase_arg_t *args = malloc(num_tasks * sizeof(chase_arg_t));

    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("Config: %s\n", label);
    printf("  Tasks: %d | Iters/task: %d | Producers/node: %d | Trials: %d\n",
        num_tasks, chase_iters, producers_per_node, TRIALS);
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    double u_times[TRIALS], t_times[TRIALS];
    long u_local_total = 0, u_remote_total = 0;
    long t_local_total = 0, t_remote_total = 0;

    for (int trial = 0; trial < TRIALS; trial++) {
        bench_result_t u = run_bench(0, weights, args, num_tasks, producers_per_node);
        bench_result_t t = run_bench(1, weights, args, num_tasks, producers_per_node);

        u_times[trial] = u.time;
        t_times[trial] = t.time;
        u_local_total += u.total_local_steals;
        u_remote_total += u.total_remote_steals;
        t_local_total += t.total_local_steals;
        t_remote_total += t.total_remote_steals;

        printf("  Trial %2d:  Uniform=%6.3fs  TopoSteal=%6.3fs  Speedup=%.2fx",
            trial + 1, u.time, t.time, u.time / t.time);
        printf("  [U: %ld local/%ld remote | T: %ld local/%ld remote]\n",
            u.total_local_steals, u.total_remote_steals,
            t.total_local_steals, t.total_remote_steals);

        if (csv) {
            fprintf(csv, "%s,%d,%d,%d,%d,%.6f,%.6f,%.4f,%ld,%ld,%ld,%ld\n",
                label, num_tasks, chase_iters, producers_per_node, trial + 1,
                u.time, t.time, u.time / t.time,
                u.total_local_steals, u.total_remote_steals,
                t.total_local_steals, t.total_remote_steals);
        }
    }

    /* Compute statistics */
    double u_sum = 0, t_sum = 0;
    for (int i = 0; i < TRIALS; i++) { u_sum += u_times[i]; t_sum += t_times[i]; }
    double u_avg = u_sum / TRIALS;
    double t_avg = t_sum / TRIALS;

    double u_var = 0, t_var = 0;
    for (int i = 0; i < TRIALS; i++) {
        u_var += (u_times[i] - u_avg) * (u_times[i] - u_avg);
        t_var += (t_times[i] - t_avg) * (t_times[i] - t_avg);
    }
    double u_std = sqrt(u_var / TRIALS);
    double t_std = sqrt(t_var / TRIALS);

    double u_min = u_times[0], u_max = u_times[0];
    double t_min = t_times[0], t_max = t_times[0];
    for (int i = 1; i < TRIALS; i++) {
        if (u_times[i] < u_min) u_min = u_times[i];
        if (u_times[i] > u_max) u_max = u_times[i];
        if (t_times[i] < t_min) t_min = t_times[i];
        if (t_times[i] > t_max) t_max = t_times[i];
    }

    long u_steals = u_local_total + u_remote_total;
    long t_steals = t_local_total + t_remote_total;

    printf("\n  %-22s  %-12s  %-12s\n", "", "Uniform", "TopoSteal");
    printf("  %-22s  %-12.4f  %-12.4f\n", "Mean (s):", u_avg, t_avg);
    printf("  %-22s  %-12.4f  %-12.4f\n", "Std dev (s):", u_std, t_std);
    printf("  %-22s  %-12.4f  %-12.4f\n", "Min (s):", u_min, t_min);
    printf("  %-22s  %-12.4f  %-12.4f\n", "Max (s):", u_max, t_max);
    printf("  %-22s  %-12.2f  %-12.2f\n", "Avg local steals:",
        u_steals ? (double)u_local_total/TRIALS : 0,
        t_steals ? (double)t_local_total/TRIALS : 0);
    printf("  %-22s  %-12.2f  %-12.2f\n", "Avg remote steals:",
        u_steals ? (double)u_remote_total/TRIALS : 0,
        t_steals ? (double)t_remote_total/TRIALS : 0);
    printf("  %-22s  %-12.1f%%  %-12.1f%%\n", "Local steal %%:",
        u_steals ? 100.0 * u_local_total / u_steals : 0,
        t_steals ? 100.0 * t_local_total / t_steals : 0);
    printf("  %-22s  %.2fx\n", "SPEEDUP:", u_avg / t_avg);
    printf("\n");

    free(args);
}

int main() {
    topo_init(&topo);
    if (topo.num_cores > NUM_WORKERS)
        topo.num_cores = NUM_WORKERS;

    weights_t weights;
    weights_init(&weights, &topo);

    size_t array_bytes = 8 * 1024 * 1024 * sizeof(size_t);

    numa0_array = mmap(NULL, array_bytes,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    touch_arg_t ta0 = { .arr = numa0_array, .count = 8*1024*1024, .cpu = topo.cpu_map[0] };
    pthread_t th0;
    pthread_create(&th0, NULL, touch_thread, &ta0);
    pthread_join(th0, NULL);
    shuffle_array(numa0_array, 8*1024*1024);

    numa1_array = mmap(NULL, array_bytes,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    touch_arg_t ta1 = { .arr = numa1_array, .count = 8*1024*1024, .cpu = topo.cpu_map[12] };
    pthread_t th1;
    pthread_create(&th1, NULL, touch_thread, &ta1);
    pthread_join(th1, NULL);
    shuffle_array(numa1_array, 8*1024*1024);

    /* Print system info */
    printf("================================================================\n");
    printf("  TopoSteal Comprehensive Benchmark Suite\n");
    printf("================================================================\n");
    printf("Platform: 2x Intel Xeon E5-2690v3, 24 cores, NUMA dist 10/21\n");
    printf("Array: 2x 64 MB (per NUMA node, exceeds 30MB L3 -> DRAM-bound)\n");
    printf("Weight function: 1/dist^2 (aggressive same-socket preference)\n");
    printf("Workers: %d pinned via hwloc cpu_map\n", NUM_WORKERS);
    printf("Trials per config: %d\n", TRIALS);
    printf("================================================================\n\n");

    topo_print(&topo);
    printf("\n");

    /* Open CSV output */
    FILE *csv = fopen("bench_results.csv", "w");
    if (csv) {
        fprintf(csv, "config,tasks,iters,producers_per_node,trial,"
                     "uniform_s,toposteal_s,speedup,"
                     "u_local_steals,u_remote_steals,t_local_steals,t_remote_steals\n");
    }

    /* Config 1: Heavy imbalance, long tasks */
    run_config("heavy-imbalance-long",
        1200, 2000000, 2, &weights, csv);

    /* Config 2: Heavy imbalance, medium tasks */
    run_config("heavy-imbalance-med",
        1200, 1000000, 2, &weights, csv);

    /* Config 3: Moderate imbalance, long tasks */
    run_config("moderate-imbalance-long",
        1200, 2000000, 4, &weights, csv);

    /* Config 4: Many short tasks, heavy imbalance */
    run_config("many-short-tasks",
        2400, 500000, 2, &weights, csv);

    /* Config 5: Extreme imbalance (1 producer per node) */
    run_config("extreme-imbalance",
        1200, 1000000, 1, &weights, csv);

    if (csv) {
        fclose(csv);
        printf("Results written to bench_results.csv\n");
    }

    printf("================================================================\n");
    printf("  Benchmark complete.\n");
    printf("================================================================\n");

    topo_destroy();
    munmap(numa0_array, array_bytes);
    munmap(numa1_array, array_bytes);
    return 0;
}
