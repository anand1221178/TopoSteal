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

#define NUM_WORKERS     24
#define SPMV_ITERS      1
#define OUTER_REPS      20
#define TRIALS          5
#define ROWS_PER_TASK   50000

/* ------------------------------------------------------------------ */
/*  CSR matrix                                                         */
/* ------------------------------------------------------------------ */
typedef struct {
    int nrows, ncols;
    long nnz;
    int *row_ptr;
    int *col_idx;
    double *values;
    double *x;
    double *y;
} csr_matrix_t;

/* ------------------------------------------------------------------ */
/*  Matrix Market loader  (COO -> CSR)                                 */
/* ------------------------------------------------------------------ */
static int load_mtx(const char *path, csr_matrix_t *mat) {
    FILE *f = fopen(path, "r");
    if (!f) { perror("fopen"); return -1; }

    char line[1024];
    int symmetric = 0;

    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    if (strstr(line, "symmetric")) symmetric = 1;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] != '%') break;
    }

    int M, N;
    long NNZ;
    if (sscanf(line, "%d %d %ld", &M, &N, &NNZ) != 3) {
        fprintf(stderr, "bad mtx header\n");
        fclose(f);
        return -1;
    }
    printf("[mtx] %d x %d, %ld entries%s\n", M, N, NNZ,
           symmetric ? " (symmetric, will mirror)" : "");

    long alloc_nnz = symmetric ? NNZ * 2 : NNZ;
    int *coo_row = malloc(alloc_nnz * sizeof(int));
    int *coo_col = malloc(alloc_nnz * sizeof(int));
    double *coo_val = malloc(alloc_nnz * sizeof(double));
    long count = 0;

    for (long k = 0; k < NNZ; k++) {
        int r, c;
        double v = 1.0;
        if (fscanf(f, "%d %d %lf", &r, &c, &v) >= 2) {
            r--; c--;
        } else {
            break;
        }
        coo_row[count] = r;
        coo_col[count] = c;
        coo_val[count] = v;
        count++;
        if (symmetric && r != c) {
            coo_row[count] = c;
            coo_col[count] = r;
            coo_val[count] = v;
            count++;
        }
    }
    fclose(f);
    printf("[mtx] %ld nonzeros after expansion\n", count);

    mat->nrows = M;
    mat->ncols = N;
    mat->nnz = count;
    mat->row_ptr = calloc(M + 1, sizeof(int));

    for (long k = 0; k < count; k++)
        mat->row_ptr[coo_row[k] + 1]++;
    for (int i = 1; i <= M; i++)
        mat->row_ptr[i] += mat->row_ptr[i - 1];

    mat->col_idx = malloc(count * sizeof(int));
    mat->values = malloc(count * sizeof(double));
    int *offset = calloc(M, sizeof(int));

    for (long k = 0; k < count; k++) {
        int r = coo_row[k];
        long pos = mat->row_ptr[r] + offset[r];
        mat->col_idx[pos] = coo_col[k];
        mat->values[pos] = coo_val[k];
        offset[r]++;
    }

    free(offset);
    free(coo_row);
    free(coo_col);
    free(coo_val);

    mat->x = malloc(N * sizeof(double));
    mat->y = calloc(M, sizeof(double));
    for (int i = 0; i < N; i++)
        mat->x[i] = 1.0 / (i + 1);

    return 0;
}

static void free_matrix(csr_matrix_t *mat) {
    free(mat->row_ptr);
    free(mat->col_idx);
    free(mat->values);
    free(mat->x);
    free(mat->y);
}

/* ------------------------------------------------------------------ */
/*  SpMV kernel: y[r0..r1) = A[r0..r1,:] * x  (single pass)          */
/* ------------------------------------------------------------------ */
typedef struct {
    csr_matrix_t *mat;
    int row_start;
    int row_end;
} spmv_task_t;

static void spmv_kernel(void *arg) {
    spmv_task_t *t = (spmv_task_t *)arg;
    csr_matrix_t *m = t->mat;
    const int *row_ptr = m->row_ptr;
    const int *col_idx = m->col_idx;
    const double *vals = m->values;
    const double *x = m->x;
    double *y = m->y;

    for (int i = t->row_start; i < t->row_end; i++) {
        double sum = 0.0;
        for (int j = row_ptr[i]; j < row_ptr[i + 1]; j++)
            sum += vals[j] * x[col_idx[j]];
        y[i] = sum;
    }
}

/* ------------------------------------------------------------------ */
/*  NUMA first-touch: pin thread to a cpu and memset the row range     */
/* ------------------------------------------------------------------ */
typedef struct {
    double *y;
    int start, end, cpu;
} touch_arg_t;

static void *touch_thread(void *arg) {
    touch_arg_t *ta = (touch_arg_t *)arg;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(ta->cpu, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    for (int i = ta->start; i < ta->end; i++)
        ta->y[i] = 0.0;
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  TopoSteal SpMV  (raw deques, 3 modes)                             */
/* ------------------------------------------------------------------ */
static topo_t topo;

static _Atomic long local_steals[128];
static _Atomic long remote_steals[128];

static int is_same_socket(int a, int b) {
    return topo.distance[a][b] < TOPO_DIST_NUMA;
}

/* Persistent worker pool with barriers (no thread create/join per rep) */
static pthread_barrier_t bar_start, bar_end;
static deque_t spmv_queues[NUM_WORKERS];
static _Atomic int spmv_tasks_done;
static int spmv_ntasks;

typedef struct {
    int id;
    int mode;
    weights_t *weights;
} worker_ctx_t;

static void *worker_fn(void *arg) {
    worker_ctx_t *ctx = (worker_ctx_t *)arg;
    unsigned int seed = (unsigned int)time(NULL) ^ ctx->id;
    task_t task;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(topo.cpu_map[ctx->id], &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    for (int rep = 0; rep < OUTER_REPS; rep++) {
        pthread_barrier_wait(&bar_start);

        while (atomic_load(&spmv_tasks_done) < spmv_ntasks) {
            if (deque_pop(&spmv_queues[ctx->id], &task)) {
                task.fn(task.arg);
                atomic_fetch_add(&spmv_tasks_done, 1);
                continue;
            }
            int victim;
            if (ctx->mode > 0) {
                victim = weights_pick_victim(ctx->weights, ctx->id, &seed);
            } else {
                victim = rand_r(&seed) % NUM_WORKERS;
            }
            if (victim >= 0 && victim != ctx->id) {
                if (deque_steal(&spmv_queues[victim], &task)) {
                    task.fn(task.arg);
                    atomic_fetch_add(&spmv_tasks_done, 1);
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

typedef struct {
    double time_s;
    double gflops;
    long local_steal_count;
    long remote_steal_count;
} spmv_result_t;

static spmv_result_t run_spmv_toposteal(csr_matrix_t *mat, int mode,
                                         weights_t *weights,
                                         spmv_task_t *tasks, int ntasks) {
    int half = mat->nrows / 2;
    spmv_ntasks = ntasks;

    for (int i = 0; i < NUM_WORKERS; i++) {
        atomic_store(&local_steals[i], 0);
        atomic_store(&remote_steals[i], 0);
    }

    pthread_barrier_init(&bar_start, NULL, NUM_WORKERS + 1);
    pthread_barrier_init(&bar_end, NULL, NUM_WORKERS + 1);

    /* Launch persistent worker pool (once) */
    worker_ctx_t ctxs[NUM_WORKERS];
    pthread_t threads[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; i++) {
        ctxs[i] = (worker_ctx_t){
            .id = i, .mode = mode, .weights = weights
        };
        pthread_create(&threads[i], NULL, worker_fn, &ctxs[i]);
    }

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int rep = 0; rep < OUTER_REPS; rep++) {
        /* Prepare work for this rep */
        for (int i = 0; i < NUM_WORKERS; i++)
            deque_init(&spmv_queues[i]);
        atomic_store(&spmv_tasks_done, 0);
        memset(mat->y, 0, mat->nrows * sizeof(double));

        for (int i = 0; i < ntasks; i++) {
            int target = (tasks[i].row_start < half) ? 0 : 12;
            task_t t = { .fn = spmv_kernel, .arg = &tasks[i] };
            deque_push(&spmv_queues[target], t);
        }

        /* Release workers */
        pthread_barrier_wait(&bar_start);
        /* Wait for completion */
        pthread_barrier_wait(&bar_end);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    for (int i = 0; i < NUM_WORKERS; i++)
        pthread_join(threads[i], NULL);

    pthread_barrier_destroy(&bar_start);
    pthread_barrier_destroy(&bar_end);

    spmv_result_t r;
    r.time_s = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    r.gflops = (2.0 * mat->nnz * OUTER_REPS) / (r.time_s * 1e9);
    r.local_steal_count = 0;
    r.remote_steal_count = 0;
    for (int i = 0; i < NUM_WORKERS; i++) {
        r.local_steal_count += atomic_load(&local_steals[i]);
        r.remote_steal_count += atomic_load(&remote_steals[i]);
    }
    return r;
}

/* ------------------------------------------------------------------ */
/*  OpenMP SpMV baseline                                               */
/* ------------------------------------------------------------------ */
static spmv_result_t run_spmv_openmp(csr_matrix_t *mat) {
    spmv_result_t r = {0};
#ifdef _OPENMP
    const int *row_ptr = mat->row_ptr;
    const int *col_idx = mat->col_idx;
    const double *vals = mat->values;
    const double *x = mat->x;
    double *y = mat->y;
    int N = mat->nrows;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int rep = 0; rep < OUTER_REPS; rep++) {
        memset(y, 0, N * sizeof(double));
        #pragma omp parallel for schedule(static) num_threads(NUM_WORKERS)
        for (int i = 0; i < N; i++) {
            double sum = 0.0;
            for (int j = row_ptr[i]; j < row_ptr[i + 1]; j++)
                sum += vals[j] * x[col_idx[j]];
            y[i] = sum;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    r.time_s = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    r.gflops = (2.0 * mat->nnz * OUTER_REPS) / (r.time_s * 1e9);
#else
    printf("  [OpenMP not available, skipping]\n");
#endif
    return r;
}

/* ------------------------------------------------------------------ */
/*  Feedback callback for PMU mode                                     */
/* ------------------------------------------------------------------ */
static void bench_feedback_cb(void *ctx) {
    feedback_t *f = (feedback_t *)ctx;
    feedback_update(f);
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <matrix.mtx>\n", argv[0]);
        return 1;
    }

    /* Load matrix */
    csr_matrix_t mat;
    if (load_mtx(argv[1], &mat) != 0) return 1;

    /* Init topology */
    topo_init(&topo);
    if (topo.num_cores > NUM_WORKERS)
        topo.num_cores = NUM_WORKERS;

    weights_t weights;
    weights_init(&weights, &topo);

    /* Init PMU */
    pmu_t pmu;
    int pmu_ok = (pmu_init(&pmu, NUM_WORKERS, topo.cpu_map) == 0);
    if (pmu_ok) {
        pmu.feedback_cb = NULL;
        pmu.feedback_ctx = NULL;
        pmu_start(&pmu);
    }

    /* NUMA first-touch y vector */
    int half = mat.nrows / 2;
    touch_arg_t ta0 = { .y = mat.y, .start = 0, .end = half,
                        .cpu = topo.cpu_map[0] };
    touch_arg_t ta1 = { .y = mat.y, .start = half, .end = mat.nrows,
                        .cpu = topo.cpu_map[12] };
    pthread_t th0, th1;
    pthread_create(&th0, NULL, touch_thread, &ta0);
    pthread_create(&th1, NULL, touch_thread, &ta1);
    pthread_join(th0, NULL);
    pthread_join(th1, NULL);

    /* Build task list */
    int ntasks = (mat.nrows + ROWS_PER_TASK - 1) / ROWS_PER_TASK;
    spmv_task_t *tasks = malloc(ntasks * sizeof(spmv_task_t));
    for (int i = 0; i < ntasks; i++) {
        tasks[i].mat = &mat;
        tasks[i].row_start = i * ROWS_PER_TASK;
        tasks[i].row_end = (i + 1) * ROWS_PER_TASK;
        if (tasks[i].row_end > mat.nrows)
            tasks[i].row_end = mat.nrows;
    }

    printf("================================================================\n");
    printf("  TopoSteal SpMV Benchmark\n");
    printf("================================================================\n");
    printf("Matrix: %s (%d rows, %ld nnz)\n", argv[1], mat.nrows, mat.nnz);
    printf("Tasks: %d (%d rows/task) | Outer reps: %d | Trials: %d\n",
           ntasks, ROWS_PER_TASK, OUTER_REPS, TRIALS);
    printf("Workers: %d | PMU: %s\n", NUM_WORKERS, pmu_ok ? "yes" : "no");
    printf("================================================================\n\n");
    topo_print(&topo);
    printf("\n");

    /* CSV output */
    FILE *csv = fopen("spmv_results.csv", "w");
    if (csv) {
        fprintf(csv, "trial,mode,time_s,gflops,local_steals,remote_steals\n");
    }

    weights_t pmu_weights;
    feedback_t pmu_fb;

    printf("  %-14s  %8s  %8s  %s\n", "Mode", "Time(s)", "GFLOP/s", "Steals(L/R)");
    printf("  %-14s  %8s  %8s  %s\n", "----", "-------", "-------", "-----------");

    double u_sum = 0, t_sum = 0, p_sum = 0, o_sum = 0;
    double u_gf = 0, t_gf = 0, p_gf = 0, o_gf = 0;

    for (int trial = 0; trial < TRIALS; trial++) {
        printf("\n--- Trial %d/%d ---\n", trial + 1, TRIALS);

        /* Uniform */
        spmv_result_t u = run_spmv_toposteal(&mat, 0, &weights, tasks, ntasks);
        printf("  %-14s  %8.4f  %8.3f  %ld/%ld\n", "Uniform",
               u.time_s, u.gflops, u.local_steal_count, u.remote_steal_count);
        u_sum += u.time_s;
        u_gf += u.gflops;

        /* TopoStatic */
        spmv_result_t t = run_spmv_toposteal(&mat, 1, &weights, tasks, ntasks);
        printf("  %-14s  %8.4f  %8.3f  %ld/%ld\n", "TopoStatic",
               t.time_s, t.gflops, t.local_steal_count, t.remote_steal_count);
        t_sum += t.time_s;
        t_gf += t.gflops;

        /* Topo+PMU */
        spmv_result_t p = {0};
        if (pmu_ok) {
            memcpy(&pmu_weights, &weights, sizeof(weights_t));
            feedback_init(&pmu_fb, &topo, &pmu_weights, &pmu);
            pmu.feedback_ctx = &pmu_fb;
            pmu.feedback_cb = bench_feedback_cb;
            p = run_spmv_toposteal(&mat, 2, &pmu_weights, tasks, ntasks);
            pmu.feedback_cb = NULL;
            printf("  %-14s  %8.4f  %8.3f  %ld/%ld\n", "Topo+PMU",
                   p.time_s, p.gflops, p.local_steal_count, p.remote_steal_count);
            p_sum += p.time_s;
            p_gf += p.gflops;
        }

        /* OpenMP */
        spmv_result_t o = run_spmv_openmp(&mat);
        if (o.time_s > 0) {
            printf("  %-14s  %8.4f  %8.3f  -\n", "OpenMP(static)",
                   o.time_s, o.gflops);
            o_sum += o.time_s;
            o_gf += o.gflops;
        }

        if (csv) {
            fprintf(csv, "%d,uniform,%.6f,%.3f,%ld,%ld\n",
                    trial + 1, u.time_s, u.gflops,
                    u.local_steal_count, u.remote_steal_count);
            fprintf(csv, "%d,topostatic,%.6f,%.3f,%ld,%ld\n",
                    trial + 1, t.time_s, t.gflops,
                    t.local_steal_count, t.remote_steal_count);
            if (pmu_ok)
                fprintf(csv, "%d,topopmu,%.6f,%.3f,%ld,%ld\n",
                        trial + 1, p.time_s, p.gflops,
                        p.local_steal_count, p.remote_steal_count);
            if (o.time_s > 0)
                fprintf(csv, "%d,openmp,%.6f,%.3f,0,0\n",
                        trial + 1, o.time_s, o.gflops);
        }
    }

    /* Summary */
    printf("\n================================================================\n");
    printf("  Summary (mean of %d trials)\n", TRIALS);
    printf("================================================================\n");
    printf("  %-14s  %8.4f s  %8.3f GFLOP/s\n", "Uniform",
           u_sum / TRIALS, u_gf / TRIALS);
    printf("  %-14s  %8.4f s  %8.3f GFLOP/s\n", "TopoStatic",
           t_sum / TRIALS, t_gf / TRIALS);
    if (pmu_ok)
        printf("  %-14s  %8.4f s  %8.3f GFLOP/s\n", "Topo+PMU",
               p_sum / TRIALS, p_gf / TRIALS);
    if (o_sum > 0)
        printf("  %-14s  %8.4f s  %8.3f GFLOP/s\n", "OpenMP(static)",
               o_sum / TRIALS, o_gf / TRIALS);

    printf("\n  Speedup vs Uniform:\n");
    printf("    TopoStatic:    %.2fx\n", u_sum / t_sum);
    if (pmu_ok)
        printf("    Topo+PMU:      %.2fx\n", u_sum / p_sum);
    if (o_sum > 0)
        printf("    OpenMP:        %.2fx\n", u_sum / o_sum);
    if (o_sum > 0 && t_sum > 0)
        printf("  TopoSteal vs OpenMP: %.2fx\n", o_sum / t_sum);

    if (csv) {
        fclose(csv);
        printf("\nResults written to spmv_results.csv\n");
    }

    if (pmu_ok) pmu_stop(&pmu);
    free(tasks);
    free_matrix(&mat);
    topo_destroy();

    printf("\n================================================================\n");
    printf("  SpMV benchmark complete.\n");
    printf("================================================================\n");
    return 0;
}
