#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include "../include/topo.h"
#include "../include/pmu.h"
#include "../include/weights.h"
#include "../include/feedback.h"

// Stressor — same as test_pmu
static void *stress_worker(void *arg) {
    int core_id = *(int *)arg;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

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
        printf("  Worker %d: %d steals (%.1f%%) miss_rate=%.1f/ms\n",
            i, counts[i],
            (float)counts[i] / trials * 100.0f,
            pmu_get_miss_rate(NULL, i));
    }
}

int main() {
    topo_t t;
    pmu_t p;
    weights_t w;
    feedback_t f;

    // Init all components
    topo_init(&t);
    if (pmu_init(&p, t.num_cores) == -1) {
        printf("[test] PMU init failed\n");
        return -1;
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

    // Start PMU
    pmu_start(&p);

    // Launch stressors on workers 2 and 3 ONLY
    int core_ids[2] = {2, 3};
    pthread_t stressors[2];
    for (int i = 0; i < 2; i++)
        pthread_create(&stressors[i], NULL, stress_worker, &core_ids[i]);

    // Wait for stressors to warm up
    usleep(500000);

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

    pmu_stop(&p);
    topo_destroy();
    return 0;
}