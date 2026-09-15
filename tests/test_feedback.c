#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#ifdef __linux__
#include <sched.h>
#endif
#include "../include/topo.h"
#include "../include/pmu.h"
#include "../include/weights.h"
#include "../include/feedback.h"

// Stressor — same as test_pmu
static void *stress_worker(void *arg) {
    int core_id = *(int *)arg;
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
#else
    (void)core_id; /* CPU pinning is Linux-only; unpinned elsewhere */
#endif

    size_t size = (32 * 1024 * 1024) / sizeof(size_t);
    size_t *arr = malloc(size * sizeof(size_t));
    for (size_t i = 0; i < size; i++) arr[i] = i;
    for (size_t i = size - 1; i > 0; i--) {
        size_t j = arr[i * 1234567891ULL % i + 1] % (i + 1);
        size_t tmp = arr[i]; arr[i] = arr[j]; arr[j] = tmp;
    }
    volatile size_t idx = 0;
    while (1) idx = arr[idx % size];
    free(arr);
    return NULL;
}

static void print_distribution(weights_t *w, topo_t *t, const char *label) {
    unsigned int seed = (unsigned int)time(NULL);
    int counts[TOPO_MAX_CORES] = {0};
    int trials = 10000;
    for (int i = 0; i < trials; i++) {
        int victim = weights_pick_victim(w, 0, &seed);
        if (victim >= 0) counts[victim]++;
    }
    printf("\n--- %s ---\n", label);
    for (int i = 0; i < t->num_cores; i++) {
        if (i == 0) { printf("  Worker 0 (self):   skipped\n"); continue; }
        printf("  Worker %d: %d steals (%.1f%%)\n",
            i, counts[i],
            (float)counts[i] / trials * 100.0f);
    }
}

int main() {
    topo_t t;
    pmu_t p;
    weights_t w;
    feedback_t f;

    // Init all components
    topo_init(&t);
    int pmu_ok = (pmu_init(&p, t.num_cores, t.cpu_map) == 0);
    if (!pmu_ok) {
        /* No real perf_event_open on this platform. feedback_update() only
         * ever reads pmu->miss_rates[] via pmu_get_miss_rate() - it doesn't
         * care whether those numbers came from real hardware counters or
         * not - so we can still exercise the actual reweighting logic by
         * injecting synthetic miss rates directly into the (public) pmu_t
         * struct below, instead of relying on real cache-miss pressure from
         * the stressor threads. This is test-only; feedback.c itself is
         * untouched. */
        printf("[test] Real PMU unavailable on this platform - will inject synthetic miss rates to exercise feedback_update() logic directly.\n");
    }
    weights_init(&w, &t);
    feedback_init(&f, &t, &w, &p);

    // Print baseline distribution
    unsigned int seed = (unsigned int)time(NULL);
    int counts[TOPO_MAX_CORES] = {0};
    for (int i = 0; i < 10000; i++) {
        int v = weights_pick_victim(&w, 0, &seed);
        if (v >= 0) counts[v]++;
    }
    printf("\n--- Baseline (no load) ---\n");
    for (int i = 1; i < t.num_cores; i++)
        printf("  Worker %d: %.1f%%\n", i, counts[i] / 10000.0f * 100.0f);

    pthread_t stressors[2];
    if (pmu_ok) {
        // Start PMU
        pmu_start(&p);

        // Launch stressors on workers 2 and 3 ONLY
        int core_ids[2] = {2, 3};
        for (int i = 0; i < 2; i++)
            pthread_create(&stressors[i], NULL, stress_worker, &core_ids[i]);

        // Wait for stressors to warm up
        usleep(500000);
    } else {
        // Synthetic stand-in for "workers 2 and 3 are heavily contended" -
        // same scenario the real stressor threads above would produce,
        // just injected directly instead of measured.
        for (int i = 0; i < t.num_cores; i++)
            atomic_store(&p.miss_rates[i], 0.0f);
        atomic_store(&p.miss_rates[2], 500.0f);
        atomic_store(&p.miss_rates[3], 450.0f);
    }

    // Run feedback update
    feedback_update(&f);

    // Print updated distribution
    memset(counts, 0, sizeof(counts));
    seed = (unsigned int)time(NULL);
    for (int i = 0; i < 10000; i++) {
        int v = weights_pick_victim(&w, 0, &seed);
        if (v >= 0) counts[v]++;
    }
    printf("\n--- After feedback (workers 2 and 3 stressed) ---\n");
    for (int i = 1; i < t.num_cores; i++) {
        printf("  Worker %d: %.1f%% (miss_rate=%.1f/ms)\n",
            i, counts[i] / 10000.0f * 100.0f,
            pmu_get_miss_rate(&p, i));
    }

    printf("\nExpected: Workers 2 and 3 should have LOWER steal probability\n");
    printf("Expected: Worker 1 should have HIGHER steal probability\n");

    if (pmu_ok) pmu_stop(&p);
    topo_destroy();
    return 0;
}