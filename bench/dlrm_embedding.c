#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <inttypes.h>
#include "../include/toposteal.h"

/*
 * DLRM-style embedding-lookup benchmark - the primary evaluation vehicle
 * for the topology- + prediction-aware proactive stealing mechanism
 * (steps 1-3), and the required 3-way ablation (step 6): does the
 * combined policy actually beat topology-only-reactive (original
 * TopoSteal) and prediction-only-topology-agnostic (A2WS-shaped) on the
 * same workload/hardware? Runs through the public toposteal API
 * (toposteal_submit/toposteal_wait), not raw deques - the older
 * benchmarks under bench/ hand-roll their own worker loops and so never
 * exercise this mechanism at all.
 *
 * What it models: a DLRM-style SparseLengthsSum embedding lookup. Each
 * "sample" gathers a variable number of rows ("pooling factor") from a
 * large shared embedding table and sums them into an output vector. This
 * is memory-bandwidth-bound (the table is far bigger than any cache) and
 * genuinely irregular - task duration scales with pooling factor, a real
 * per-task feature, not just execution-order noise.
 *
 * v1 simplifications, called out explicitly rather than hidden:
 *   - pooling factor is uniform-random in [MIN,MAX], not a realistic
 *     access-pattern distribution (e.g. Zipfian category skew).
 *   - the embedding table has no NUMA-aware placement (plain malloc) -
 *     real multi-socket sensitivity needs first-touch/libnuma placement,
 *     deferred to later cluster evaluation.
 */

#define EMBED_ROWS   2000000   /* table rows */
#define EMBED_DIM    32        /* floats per row -> table is ~256MB, well beyond any cache */
#define NUM_SAMPLES  20000     /* number of lookup tasks */
#define MIN_POOLING  4
#define MAX_POOLING  64
#define NUM_WORKERS  4

typedef struct {
    int pooling_factor;
    int *indices;   /* pooling_factor row-indices into g_table */
    float out[EMBED_DIM];
} embed_task_t;

static float *g_table;          /* EMBED_ROWS * EMBED_DIM */
static embed_task_t *g_tasks;   /* NUM_SAMPLES */
static float *g_ref_out;        /* NUM_SAMPLES * EMBED_DIM, serial ground truth */

static void embed_lookup_kernel(void *arg) {
    embed_task_t *task = (embed_task_t *)arg;
    for (int d = 0; d < EMBED_DIM; d++) task->out[d] = 0.0f;
    for (int p = 0; p < task->pooling_factor; p++) {
        const float *row = &g_table[(size_t)task->indices[p] * EMBED_DIM];
        for (int d = 0; d < EMBED_DIM; d++)
            task->out[d] += row[d];
    }
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ponytail: RAPL energy is opportunistic/secondary only (per the plan -
 * never a scheduling input, never required for correctness). Reads
 * package-0's sysfs counter if present (Linux + Intel/AMD RAPL); reports
 * "unavailable" otherwise rather than guessing. Doesn't handle multiple
 * packages - add intel-rapl:1, :2, ... if run on a multi-socket box. */
static int read_energy_uj(uint64_t *out) {
    FILE *f = fopen("/sys/class/powercap/intel-rapl:0/energy_uj", "r");
    if (!f) return 0;
    int ok = (fscanf(f, "%" SCNu64, out) == 1);
    fclose(f);
    return ok;
}

static uint64_t rapl_max_range_uj(void) {
    FILE *f = fopen("/sys/class/powercap/intel-rapl:0/max_energy_range_uj", "r");
    if (!f) return 0;
    uint64_t v = 0;
    if (fscanf(f, "%" SCNu64, &v) != 1) v = 0;
    fclose(f);
    return v;
}

typedef struct {
    const char *label;
    double elapsed_s;
    int mismatches;
    double joules;   /* -1 if RAPL unavailable */
} trial_result_t;

static trial_result_t run_trial(int policy, const char *label) {
    for (int i = 0; i < NUM_SAMPLES; i++)
        memset(g_tasks[i].out, 0, sizeof(g_tasks[i].out));

    printf("\n=== policy: %s ===\n", label);
    toposteal_t *ts = toposteal_init_policy(NUM_WORKERS, policy);

    uint64_t e_start = 0, e_end = 0;
    int have_energy = read_energy_uj(&e_start);

    uint64_t start = now_ns();
    for (int i = 0; i < NUM_SAMPLES; i++)
        toposteal_submit(ts, embed_lookup_kernel, &g_tasks[i]);
    toposteal_wait(ts);
    uint64_t elapsed_ns = now_ns() - start;

    if (have_energy) have_energy = read_energy_uj(&e_end);

    toposteal_print_latency_stats(ts);
    toposteal_destroy(ts);

    int mismatches = 0;
    for (int i = 0; i < NUM_SAMPLES; i++)
        for (int d = 0; d < EMBED_DIM; d++)
            if (g_tasks[i].out[d] != g_ref_out[(size_t)i * EMBED_DIM + d]) { mismatches++; break; }

    double joules = -1.0;
    if (have_energy) {
        uint64_t max_range = rapl_max_range_uj();
        uint64_t delta_uj = (e_end >= e_start) ? (e_end - e_start) : (max_range - e_start + e_end);
        joules = delta_uj / 1e6;
    }

    trial_result_t r = { label, (double)elapsed_ns / 1e9, mismatches, joules };
    printf("[dlrm] %-20s wall=%.4fs  throughput=%.0f samples/s  correctness=%s (%d/%d)",
        label, r.elapsed_s, NUM_SAMPLES / r.elapsed_s,
        mismatches == 0 ? "PASSED" : "FAILED", mismatches, NUM_SAMPLES);
    if (have_energy) printf("  energy=%.3fJ (%.0f samples/J)", joules, NUM_SAMPLES / joules);
    else printf("  energy=n/a (no RAPL sysfs node - not this platform/needs read perms)");
    printf("\n");
    return r;
}

/* Generates NUM_SAMPLES tasks, round-robin-submitted so sample i lands on
 * worker (i % NUM_WORKERS). skewed=0: uniform-random pooling factor for
 * everyone (the "nothing to fix" regime - is combined harmless when
 * balanced already?). skewed=1: worker slot 0 gets heavy pooling
 * ([200,256], ~10x the work), the other three stay light ([4,16]) - a
 * genuinely imbalanced regime (the regime the mechanism is actually for)
 * so the ablation tests both sides of H6, not just the easy one. */
static void generate_tasks(int skewed) {
    for (int i = 0; i < NUM_SAMPLES; i++) {
        int p;
        if (skewed && i % NUM_WORKERS == 0) p = 200 + rand() % 57;
        else if (skewed)                    p = 4 + rand() % 13;
        else                                p = MIN_POOLING + rand() % (MAX_POOLING - MIN_POOLING + 1);

        g_tasks[i].pooling_factor = p;
        free(g_tasks[i].indices);
        g_tasks[i].indices = malloc((size_t)p * sizeof(int));
        for (int k = 0; k < p; k++)
            g_tasks[i].indices[k] = rand() % EMBED_ROWS;
        memset(g_tasks[i].out, 0, sizeof(g_tasks[i].out));
    }
}

static void compute_reference(void) {
    for (int i = 0; i < NUM_SAMPLES; i++) {
        embed_task_t tmp = g_tasks[i];
        embed_lookup_kernel(&tmp);
        memcpy(&g_ref_out[(size_t)i * EMBED_DIM], tmp.out, EMBED_DIM * sizeof(float));
    }
}

static int run_ablation(const char *scenario_name, int skewed) {
    printf("\n############ scenario: %s ############\n", scenario_name);
    generate_tasks(skewed);
    compute_reference();

    trial_result_t results[3];
    results[0] = run_trial(TOPOSTEAL_POLICY_TOPOLOGY_REACTIVE, "topology-reactive");
    results[1] = run_trial(TOPOSTEAL_POLICY_PREDICTION_ONLY, "prediction-only");
    results[2] = run_trial(TOPOSTEAL_POLICY_COMBINED, "combined");

    printf("\n--- %s: 3-way ablation summary ---\n", scenario_name);
    printf("%-20s %12s %15s %14s %12s\n", "policy", "wall (s)", "samples/s", "energy (J)", "correct");
    int any_fail = 0;
    for (int i = 0; i < 3; i++) {
        if (results[i].joules >= 0)
            printf("%-20s %12.4f %15.0f %14.3f %12s\n",
                results[i].label, results[i].elapsed_s, NUM_SAMPLES / results[i].elapsed_s,
                results[i].joules, results[i].mismatches == 0 ? "yes" : "NO");
        else
            printf("%-20s %12.4f %15.0f %14s %12s\n",
                results[i].label, results[i].elapsed_s, NUM_SAMPLES / results[i].elapsed_s,
                "n/a", results[i].mismatches == 0 ? "yes" : "NO");
        any_fail |= (results[i].mismatches != 0);
    }
    double speedup = results[0].elapsed_s / results[2].elapsed_s;
    printf("combined vs topology-reactive: %.2fx %s\n", speedup, speedup >= 1.0 ? "faster" : "SLOWER");
    if (results[0].joules >= 0 && results[2].joules >= 0) {
        double e_ratio = results[0].joules / results[2].joules;
        printf("combined vs topology-reactive energy: %.2fx %s\n", e_ratio, e_ratio >= 1.0 ? "more efficient" : "LESS efficient");
    }
    return any_fail;
}

int main(void) {
    srand(42); /* reproducible input generation */

    printf("[dlrm] allocating embedding table: %d rows x %d dims (%.1f MB)\n",
           EMBED_ROWS, EMBED_DIM, (double)EMBED_ROWS * EMBED_DIM * sizeof(float) / (1024.0 * 1024.0));
    g_table = malloc((size_t)EMBED_ROWS * EMBED_DIM * sizeof(float));
    if (!g_table) { fprintf(stderr, "[dlrm] table allocation failed\n"); return 1; }
    for (size_t r = 0; r < (size_t)EMBED_ROWS; r++)
        for (int d = 0; d < EMBED_DIM; d++)
            g_table[r * EMBED_DIM + d] = (float)(r % 1000) + (float)d * 0.01f;

    g_tasks = calloc((size_t)NUM_SAMPLES, sizeof(embed_task_t));
    g_ref_out = malloc((size_t)NUM_SAMPLES * EMBED_DIM * sizeof(float));

    int any_fail = 0;
    any_fail |= run_ablation("balanced (uniform pooling)", 0);
    any_fail |= run_ablation("skewed (worker 0 gets 10x heavier tasks)", 1);

    for (int i = 0; i < NUM_SAMPLES; i++) free(g_tasks[i].indices);
    free(g_tasks);
    free(g_ref_out);
    free(g_table);

    return any_fail ? 1 : 0;
}
