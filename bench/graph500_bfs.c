#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include "../include/topo.h"
#include "../include/deque.h"
#include "../include/weights.h"
#include "../include/pmu.h"
#include "../include/feedback.h"

#ifdef _OPENMP
#include <omp.h>
#endif

/* ------------------------------------------------------------------ */
/*  Graph500 parameters                                                */
/* ------------------------------------------------------------------ */
#define NUM_WORKERS     24
#define SCALE           20        /* 2^20 = ~1M vertices */
#define EDGE_FACTOR     16        /* 16M edges */
#define NUM_BFS_ROOTS   16        /* number of BFS searches */
#define FRONTIER_CHUNK  4096      /* vertices per task */
#define TRIALS          3

/* R-MAT parameters (Graph500 spec: A=0.57, B=C=0.19, D=0.05) */
#define RMAT_A  0.57
#define RMAT_B  0.19
#define RMAT_C  0.19
/* RMAT_D = 1 - A - B - C = 0.05 */

/* ------------------------------------------------------------------ */
/*  CSR graph                                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    long nvertices;
    long nedges;
    long *row_ptr;   /* size nvertices+1 */
    long *col_idx;   /* size nedges */
} csr_graph_t;

/* ------------------------------------------------------------------ */
/*  R-MAT Kronecker graph generator (Graph500 spec)                    */
/* ------------------------------------------------------------------ */
static void generate_rmat_edges(long scale, long nedges,
                                long *src, long *dst, unsigned int *seed) {
    long nvertices = 1L << scale;
    double a = RMAT_A, b = RMAT_B, c = RMAT_C;

    for (long k = 0; k < nedges; k++) {
        long u = 0, v = 0;
        for (long bit = scale - 1; bit >= 0; bit--) {
            double r = (double)rand_r(seed) / RAND_MAX;
            long mask = 1L << bit;
            if (r < a) {
                /* quadrant (0,0) */
            } else if (r < a + b) {
                v |= mask;
            } else if (r < a + b + c) {
                u |= mask;
            } else {
                u |= mask;
                v |= mask;
            }
            /* Noise: slight perturbation (Graph500 spec) */
            double noise = 0.1;
            a *= (1.0 - noise + noise * (double)rand_r(seed) / RAND_MAX);
            b *= (1.0 - noise + noise * (double)rand_r(seed) / RAND_MAX);
            c *= (1.0 - noise + noise * (double)rand_r(seed) / RAND_MAX);
            double d_val = 1.0 - a - b - c;
            if (d_val < 0.01) d_val = 0.01;
            double norm = 1.0 / (a + b + c + d_val);
            a *= norm; b *= norm; c *= norm;
        }
        /* Reset parameters for next edge */
        a = RMAT_A; b = RMAT_B; c = RMAT_C;
        src[k] = u % nvertices;
        dst[k] = v % nvertices;
    }
}

static void build_csr(long nvertices, long nedges_input,
                      long *src, long *dst, csr_graph_t *g) {
    /* Count: undirected, so add both directions; skip self-loops */
    long *degree = calloc(nvertices, sizeof(long));
    long actual = 0;
    for (long k = 0; k < nedges_input; k++) {
        if (src[k] != dst[k]) {
            degree[src[k]]++;
            degree[dst[k]]++;
            actual += 2;
        }
    }

    g->nvertices = nvertices;
    g->nedges = actual;
    g->row_ptr = malloc((nvertices + 1) * sizeof(long));
    g->row_ptr[0] = 0;
    for (long i = 1; i <= nvertices; i++)
        g->row_ptr[i] = g->row_ptr[i - 1] + degree[i - 1];

    g->col_idx = malloc(actual * sizeof(long));
    long *offset = calloc(nvertices, sizeof(long));
    for (long k = 0; k < nedges_input; k++) {
        if (src[k] != dst[k]) {
            long u = src[k], v = dst[k];
            g->col_idx[g->row_ptr[u] + offset[u]++] = v;
            g->col_idx[g->row_ptr[v] + offset[v]++] = u;
        }
    }
    free(offset);
    free(degree);
}

/* ------------------------------------------------------------------ */
/*  BFS level array + frontier                                         */
/* ------------------------------------------------------------------ */
static _Atomic int *bfs_level;     /* per-vertex level (-1 = unvisited) */
static long *frontier_in;          /* current frontier vertices */
static long *frontier_out;         /* next frontier vertices */
static _Atomic long frontier_out_tail;

/* ------------------------------------------------------------------ */
/*  BFS task: process a chunk of the current frontier                   */
/* ------------------------------------------------------------------ */
typedef struct {
    csr_graph_t *g;
    long start;         /* index into frontier_in */
    long end;
    int current_level;
} bfs_task_t;

static void bfs_chunk_kernel(void *arg) {
    bfs_task_t *t = (bfs_task_t *)arg;
    csr_graph_t *g = t->g;
    const long *row_ptr = g->row_ptr;
    const long *col_idx = g->col_idx;

    for (long i = t->start; i < t->end; i++) {
        long v = frontier_in[i];
        for (long j = row_ptr[v]; j < row_ptr[v + 1]; j++) {
            long w = col_idx[j];
            int expected = -1;
            if (atomic_compare_exchange_strong(&bfs_level[w],
                                               &expected, t->current_level)) {
                long pos = atomic_fetch_add(&frontier_out_tail, 1);
                frontier_out[pos] = w;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  TopoSteal infrastructure (persistent workers with barriers)        */
/* ------------------------------------------------------------------ */
static topo_t topo;
static pthread_barrier_t bar_start, bar_end;
static deque_t bfs_queues[NUM_WORKERS];
static _Atomic int bfs_tasks_done;
static int bfs_ntasks;

static _Atomic long local_steals[128];
static _Atomic long remote_steals[128];

static int is_same_socket(int a, int b) {
    return topo.distance[a][b] < TOPO_DIST_NUMA;
}

typedef struct {
    int id;
    int mode;
    weights_t *weights;
    int total_reps;
} worker_ctx_t;

static void *worker_fn(void *arg) {
    worker_ctx_t *ctx = (worker_ctx_t *)arg;
    unsigned int seed = (unsigned int)time(NULL) ^ ctx->id;
    task_t task;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(topo.cpu_map[ctx->id], &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    for (int rep = 0; rep < ctx->total_reps; rep++) {
        pthread_barrier_wait(&bar_start);

        while (atomic_load(&bfs_tasks_done) < bfs_ntasks) {
            if (deque_pop(&bfs_queues[ctx->id], &task)) {
                task.fn(task.arg);
                atomic_fetch_add(&bfs_tasks_done, 1);
                continue;
            }
            int victim;
            if (ctx->mode > 0) {
                victim = weights_pick_victim(ctx->weights, ctx->id, &seed);
            } else {
                victim = rand_r(&seed) % NUM_WORKERS;
            }
            if (victim >= 0 && victim != ctx->id) {
                if (deque_steal(&bfs_queues[victim], &task)) {
                    task.fn(task.arg);
                    atomic_fetch_add(&bfs_tasks_done, 1);
                    if (is_same_socket(ctx->id, victim))
                        atomic_fetch_add(&local_steals[ctx->id], 1);
                    else
                        atomic_fetch_add(&remote_steals[ctx->id], 1);
                }
            }
        }

        pthread_barrier_wait(&bar_end);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Run one full BFS using TopoSteal                                   */
/* ------------------------------------------------------------------ */
typedef struct {
    double time_s;
    double mteps;
    long edges_traversed;
    long local_steal_count;
    long remote_steal_count;
} bfs_result_t;

/* Max tasks per BFS level — generous upper bound */
#define MAX_TASKS_PER_LEVEL 1024

static bfs_result_t run_bfs_toposteal(csr_graph_t *g, long root, int mode,
                                       weights_t *weights) {
    long nv = g->nvertices;

    /* Reset level array */
    for (long i = 0; i < nv; i++)
        atomic_store(&bfs_level[i], -1);
    atomic_store(&bfs_level[root], 0);

    for (int i = 0; i < NUM_WORKERS; i++) {
        atomic_store(&local_steals[i], 0);
        atomic_store(&remote_steals[i], 0);
    }

    /* Seed frontier with root */
    frontier_in[0] = root;
    long frontier_size = 1;
    int level = 1;
    long total_edges = 0;

    /* Pre-allocate task array */
    bfs_task_t *tasks = malloc(MAX_TASKS_PER_LEVEL * sizeof(bfs_task_t));

    /* Count total BFS levels first to tell workers how many reps */
    /* We can't know in advance, so we do BFS iteratively from main thread,
       using barriers to dispatch each level as one "rep" */

    /* Actually, we need a different approach: run BFS level-by-level,
       each level = one barrier round */

    /* First, count levels with a serial BFS to know total_reps */
    int *serial_level = malloc(nv * sizeof(int));
    memset(serial_level, -1, nv * sizeof(int));
    serial_level[root] = 0;
    long *serial_q = malloc(nv * sizeof(long));
    long qhead = 0, qtail = 0;
    serial_q[qtail++] = root;
    int max_level = 0;
    while (qhead < qtail) {
        long v = serial_q[qhead++];
        for (long j = g->row_ptr[v]; j < g->row_ptr[v + 1]; j++) {
            long w = g->col_idx[j];
            if (serial_level[w] == -1) {
                serial_level[w] = serial_level[v] + 1;
                serial_q[qtail++] = w;
                if (serial_level[w] > max_level)
                    max_level = serial_level[w];
            }
        }
    }
    free(serial_level);
    free(serial_q);

    int total_levels = max_level; /* number of barrier rounds needed */

    /* Reset level array again for real BFS */
    for (long i = 0; i < nv; i++)
        atomic_store(&bfs_level[i], -1);
    atomic_store(&bfs_level[root], 0);
    frontier_in[0] = root;
    frontier_size = 1;
    level = 1;

    /* Launch workers */
    pthread_barrier_init(&bar_start, NULL, NUM_WORKERS + 1);
    pthread_barrier_init(&bar_end, NULL, NUM_WORKERS + 1);

    worker_ctx_t ctxs[NUM_WORKERS];
    pthread_t threads[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; i++) {
        ctxs[i] = (worker_ctx_t){
            .id = i, .mode = mode, .weights = weights,
            .total_reps = total_levels
        };
        pthread_create(&threads[i], NULL, worker_fn, &ctxs[i]);
    }

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (frontier_size > 0 && level <= max_level) {
        /* Build tasks for this level */
        int ntasks = (int)((frontier_size + FRONTIER_CHUNK - 1) / FRONTIER_CHUNK);
        if (ntasks > MAX_TASKS_PER_LEVEL) ntasks = MAX_TASKS_PER_LEVEL;

        for (int i = 0; i < NUM_WORKERS; i++)
            deque_init(&bfs_queues[i]);
        atomic_store(&bfs_tasks_done, 0);
        atomic_store(&frontier_out_tail, 0);
        bfs_ntasks = ntasks;

        long chunk = (frontier_size + ntasks - 1) / ntasks;
        int half_v = (int)(g->nvertices / 2);

        for (int i = 0; i < ntasks; i++) {
            tasks[i].g = g;
            tasks[i].start = i * chunk;
            tasks[i].end = (i + 1) * chunk;
            if (tasks[i].end > frontier_size)
                tasks[i].end = frontier_size;
            tasks[i].current_level = level;

            /* NUMA-aware placement based on frontier vertex IDs */
            long sample_v = frontier_in[tasks[i].start];
            int target = (sample_v < half_v) ? 0 : 12;
            task_t t = { .fn = bfs_chunk_kernel, .arg = &tasks[i] };
            deque_push(&bfs_queues[target], t);
        }

        /* Count edges in this level's frontier */
        for (long i = 0; i < frontier_size; i++)
            total_edges += g->row_ptr[frontier_in[i] + 1] -
                           g->row_ptr[frontier_in[i]];

        /* Release workers and wait */
        pthread_barrier_wait(&bar_start);
        pthread_barrier_wait(&bar_end);

        /* Swap frontiers */
        long *tmp = frontier_in;
        frontier_in = frontier_out;
        frontier_out = tmp;
        frontier_size = atomic_load(&frontier_out_tail);
        level++;
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    for (int i = 0; i < NUM_WORKERS; i++)
        pthread_join(threads[i], NULL);
    pthread_barrier_destroy(&bar_start);
    pthread_barrier_destroy(&bar_end);
    free(tasks);

    bfs_result_t r;
    r.time_s = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    r.edges_traversed = total_edges;
    r.mteps = (double)total_edges / (r.time_s * 1e6);
    r.local_steal_count = 0;
    r.remote_steal_count = 0;
    for (int i = 0; i < NUM_WORKERS; i++) {
        r.local_steal_count += atomic_load(&local_steals[i]);
        r.remote_steal_count += atomic_load(&remote_steals[i]);
    }
    return r;
}

/* ------------------------------------------------------------------ */
/*  OpenMP BFS baseline (level-synchronous)                            */
/* ------------------------------------------------------------------ */
static bfs_result_t run_bfs_openmp(csr_graph_t *g, long root) {
    bfs_result_t r = {0};
#ifdef _OPENMP
    long nv = g->nvertices;
    int *level_arr = malloc(nv * sizeof(int));
    memset(level_arr, -1, nv * sizeof(int));
    level_arr[root] = 0;

    long *fin = malloc(nv * sizeof(long));
    long *fout = malloc(nv * sizeof(long));
    _Atomic long fout_tail;
    atomic_store(&fout_tail, 0);

    fin[0] = root;
    long fsize = 1;
    int lev = 1;
    long total_edges = 0;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (fsize > 0) {
        atomic_store(&fout_tail, 0);

        /* Count edges */
        for (long i = 0; i < fsize; i++)
            total_edges += g->row_ptr[fin[i] + 1] - g->row_ptr[fin[i]];

        #pragma omp parallel for schedule(dynamic, 256) num_threads(NUM_WORKERS)
        for (long i = 0; i < fsize; i++) {
            long v = fin[i];
            for (long j = g->row_ptr[v]; j < g->row_ptr[v + 1]; j++) {
                long w = g->col_idx[j];
                int expected = -1;
                if (__atomic_compare_exchange_n(&level_arr[w], &expected, lev,
                                                0, __ATOMIC_SEQ_CST,
                                                __ATOMIC_SEQ_CST)) {
                    long pos = atomic_fetch_add(&fout_tail, 1);
                    fout[pos] = w;
                }
            }
        }

        long *tmp = fin;
        fin = fout;
        fout = tmp;
        fsize = atomic_load(&fout_tail);
        lev++;
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    r.time_s = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    r.edges_traversed = total_edges;
    r.mteps = (double)total_edges / (r.time_s * 1e6);

    free(level_arr);
    free(fin);
    free(fout);
#else
    printf("  [OpenMP not available, skipping]\n");
#endif
    return r;
}

/* ------------------------------------------------------------------ */
/*  Find high-degree BFS roots (Graph500 spec: sample from max-degree) */
/* ------------------------------------------------------------------ */
static void find_bfs_roots(csr_graph_t *g, long *roots, int nroots) {
    /* Find vertices with highest degree */
    long *degrees = malloc(g->nvertices * sizeof(long));
    for (long i = 0; i < g->nvertices; i++)
        degrees[i] = g->row_ptr[i + 1] - g->row_ptr[i];

    /* Pick top-degree vertices, spread across vertex ID range */
    long stride = g->nvertices / (nroots * 2);
    int found = 0;
    for (int pass = 0; found < nroots; pass++) {
        long best = -1, best_deg = 0;
        long region_start = (found * g->nvertices) / nroots;
        long region_end = ((found + 1) * g->nvertices) / nroots;
        for (long i = region_start; i < region_end; i++) {
            if (degrees[i] > best_deg) {
                best_deg = degrees[i];
                best = i;
            }
        }
        if (best >= 0 && best_deg > 0)
            roots[found++] = best;
        else {
            /* Fallback: pick any vertex with edges */
            for (long i = 0; i < g->nvertices && found < nroots; i++) {
                if (degrees[i] > 0) roots[found++] = i;
            }
        }
    }
    free(degrees);
}

/* ------------------------------------------------------------------ */
/*  PMU feedback callback                                              */
/* ------------------------------------------------------------------ */
static void bench_feedback_cb(void *ctx) {
    feedback_t *f = (feedback_t *)ctx;
    feedback_update(f);
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */
int main() {
    long nvertices = 1L << SCALE;
    long nedges_input = nvertices * EDGE_FACTOR;

    printf("================================================================\n");
    printf("  TopoSteal Graph500 BFS Benchmark\n");
    printf("================================================================\n");
    printf("SCALE=%d (%ld vertices), edge_factor=%d (%ld directed edges)\n",
           SCALE, nvertices, EDGE_FACTOR, nedges_input);
    printf("Frontier chunk: %d | BFS roots: %d | Trials: %d\n",
           FRONTIER_CHUNK, NUM_BFS_ROOTS, TRIALS);
    printf("================================================================\n\n");

    /* Generate R-MAT edges */
    printf("[graph] Generating R-MAT Kronecker graph...\n");
    long *src = malloc(nedges_input * sizeof(long));
    long *dst = malloc(nedges_input * sizeof(long));
    unsigned int seed = 12345;
    generate_rmat_edges(SCALE, nedges_input, src, dst, &seed);

    /* Build CSR */
    printf("[graph] Building CSR (undirected)...\n");
    csr_graph_t g;
    build_csr(nvertices, nedges_input, src, dst, &g);
    free(src);
    free(dst);
    printf("[graph] %ld vertices, %ld edges (undirected)\n",
           g.nvertices, g.nedges);

    /* Degree stats */
    long max_deg = 0, isolated = 0;
    for (long i = 0; i < nvertices; i++) {
        long d = g.row_ptr[i + 1] - g.row_ptr[i];
        if (d > max_deg) max_deg = d;
        if (d == 0) isolated++;
    }
    printf("[graph] max degree: %ld, isolated vertices: %ld\n\n",
           max_deg, isolated);

    /* Init topology */
    topo_init(&topo);
    if (topo.num_cores > NUM_WORKERS)
        topo.num_cores = NUM_WORKERS;
    topo_print(&topo);
    printf("\n");

    weights_t weights;
    weights_init(&weights, &topo);

    pmu_t pmu;
    int pmu_ok = (pmu_init(&pmu, NUM_WORKERS, topo.cpu_map) == 0);
    if (pmu_ok) {
        pmu.feedback_cb = NULL;
        pmu.feedback_ctx = NULL;
        pmu_start(&pmu);
    }

    /* Allocate BFS arrays */
    bfs_level = malloc(nvertices * sizeof(_Atomic int));
    frontier_in = malloc(nvertices * sizeof(long));
    frontier_out = malloc(nvertices * sizeof(long));

    /* Find BFS roots */
    long roots[NUM_BFS_ROOTS];
    find_bfs_roots(&g, roots, NUM_BFS_ROOTS);
    printf("BFS roots (vertex ID): ");
    for (int i = 0; i < NUM_BFS_ROOTS; i++)
        printf("%ld ", roots[i]);
    printf("\n\n");

    /* CSV output */
    FILE *csv = fopen("graph500_results.csv", "w");
    if (csv) {
        fprintf(csv, "trial,root,mode,time_s,mteps,edges,local_steals,remote_steals\n");
    }

    weights_t pmu_weights;
    feedback_t pmu_fb;

    printf("  %-14s  %8s  %10s  %s\n", "Mode", "Time(s)", "MTEPS", "Steals(L/R)");
    printf("  %-14s  %8s  %10s  %s\n", "----", "-------", "-----", "-----------");

    double u_mteps_sum = 0, t_mteps_sum = 0, p_mteps_sum = 0, o_mteps_sum = 0;
    int total_runs = 0;

    for (int trial = 0; trial < TRIALS; trial++) {
        printf("\n=== Trial %d/%d ===\n", trial + 1, TRIALS);

        for (int ri = 0; ri < NUM_BFS_ROOTS; ri++) {
            long root = roots[ri];
            printf("  Root %ld (deg=%ld):\n", root,
                   g.row_ptr[root + 1] - g.row_ptr[root]);

            /* Uniform */
            bfs_result_t u = run_bfs_toposteal(&g, root, 0, &weights);
            printf("    %-14s  %8.4f  %10.1f  %ld/%ld\n", "Uniform",
                   u.time_s, u.mteps, u.local_steal_count, u.remote_steal_count);
            u_mteps_sum += u.mteps;

            /* TopoStatic */
            bfs_result_t t = run_bfs_toposteal(&g, root, 1, &weights);
            printf("    %-14s  %8.4f  %10.1f  %ld/%ld\n", "TopoStatic",
                   t.time_s, t.mteps, t.local_steal_count, t.remote_steal_count);
            t_mteps_sum += t.mteps;

            /* Topo+PMU */
            bfs_result_t p = {0};
            if (pmu_ok) {
                memcpy(&pmu_weights, &weights, sizeof(weights_t));
                feedback_init(&pmu_fb, &topo, &pmu_weights, &pmu);
                pmu.feedback_ctx = &pmu_fb;
                pmu.feedback_cb = bench_feedback_cb;
                p = run_bfs_toposteal(&g, root, 2, &pmu_weights);
                pmu.feedback_cb = NULL;
                printf("    %-14s  %8.4f  %10.1f  %ld/%ld\n", "Topo+PMU",
                       p.time_s, p.mteps, p.local_steal_count, p.remote_steal_count);
                p_mteps_sum += p.mteps;
            }

            /* OpenMP */
            bfs_result_t o = run_bfs_openmp(&g, root);
            if (o.time_s > 0) {
                printf("    %-14s  %8.4f  %10.1f  -\n", "OpenMP(dyn)",
                       o.time_s, o.mteps);
                o_mteps_sum += o.mteps;
            }

            if (csv) {
                fprintf(csv, "%d,%ld,uniform,%.6f,%.1f,%ld,%ld,%ld\n",
                        trial + 1, root, u.time_s, u.mteps, u.edges_traversed,
                        u.local_steal_count, u.remote_steal_count);
                fprintf(csv, "%d,%ld,topostatic,%.6f,%.1f,%ld,%ld,%ld\n",
                        trial + 1, root, t.time_s, t.mteps, t.edges_traversed,
                        t.local_steal_count, t.remote_steal_count);
                if (pmu_ok)
                    fprintf(csv, "%d,%ld,topopmu,%.6f,%.1f,%ld,%ld,%ld\n",
                            trial + 1, root, p.time_s, p.mteps, p.edges_traversed,
                            p.local_steal_count, p.remote_steal_count);
                if (o.time_s > 0)
                    fprintf(csv, "%d,%ld,openmp,%.6f,%.1f,%ld,0,0\n",
                            trial + 1, root, o.time_s, o.mteps, o.edges_traversed);
            }

            total_runs++;
        }
    }

    /* Summary */
    printf("\n================================================================\n");
    printf("  Summary (mean of %d BFS runs)\n", total_runs * TRIALS);
    printf("================================================================\n");
    printf("  %-14s  %10.1f MTEPS\n", "Uniform", u_mteps_sum / total_runs);
    printf("  %-14s  %10.1f MTEPS\n", "TopoStatic", t_mteps_sum / total_runs);
    if (pmu_ok)
        printf("  %-14s  %10.1f MTEPS\n", "Topo+PMU", p_mteps_sum / total_runs);
    if (o_mteps_sum > 0)
        printf("  %-14s  %10.1f MTEPS\n", "OpenMP(dyn)", o_mteps_sum / total_runs);

    double t_mean = t_mteps_sum / total_runs;
    double u_mean = u_mteps_sum / total_runs;
    double o_mean = o_mteps_sum / total_runs;
    printf("\n  Speedup vs Uniform:\n");
    printf("    TopoStatic:    %.2fx\n", t_mean / u_mean);
    if (pmu_ok)
        printf("    Topo+PMU:      %.2fx\n", (p_mteps_sum / total_runs) / u_mean);
    if (o_mean > 0) {
        printf("    OpenMP:        %.2fx\n", o_mean / u_mean);
        printf("  TopoSteal vs OpenMP: %.2fx\n", t_mean / o_mean);
    }

    if (csv) {
        fclose(csv);
        printf("\nResults written to graph500_results.csv\n");
    }

    if (pmu_ok) pmu_stop(&pmu);
    free(bfs_level);
    free(frontier_in);
    free(frontier_out);
    free(g.row_ptr);
    free(g.col_idx);
    topo_destroy();

    printf("\n================================================================\n");
    printf("  Graph500 BFS benchmark complete.\n");
    printf("================================================================\n");
    return 0;
}
