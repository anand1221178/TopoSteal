#define _GNU_SOURCE    // must be FIRST line before any includes
#include <stdio.h>
#include <unistd.h>
#include "../include/pmu.h"
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>

// Stressor thread - randomly accesses a large array to generate cache misses
static void *stress_worker(void *arg) {
    int core_id = *(int *)arg;
    
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    
    // 32MB — still bigger than L3 (8MB), init is instant
    size_t size = (32 * 1024 * 1024) / sizeof(size_t);
    size_t *arr = malloc(size * sizeof(size_t));
    
    // Random permutation — true pointer chase, defeats prefetcher
    for (size_t i = 0; i < size; i++) arr[i] = i;
    // Shuffle
    for (size_t i = size - 1; i > 0; i--) {
        size_t j = arr[i * 1234567891ULL % i + 1] % (i + 1);
        size_t tmp = arr[i]; arr[i] = arr[j]; arr[j] = tmp;
    }
    
    // Chase — volatile forces actual memory reads
    volatile size_t idx = 0;
    while (1) {
        idx = arr[idx % size];
    }
    
    free(arr);
    return NULL;
}

int main() {
    pmu_t pmu;
    
    //Init with 4 workers
    if (pmu_init(&pmu, 4) == -1) {
        printf("[test] PMU failed to initialize. Exiting.\n");
        return -1;
    }
    int core_ids[4] = {0, 1, 2, 3};
    pthread_t stressors[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&stressors[i], NULL, stress_worker, &core_ids[i]);
    
    // tart the background thread
    pmu_start(&pmu);
    
    /* Sample for 500 ms */
    usleep(2000000);
    
    // print miss rates for all 4 workers
    for (int sample = 0; sample < 5; sample++) {
    usleep(200000);
    printf("\n--- PMU Readings (sample %d) ---\n", sample);
    for (int i = 0; i < 4; i++) {
        float rate = pmu_get_miss_rate(&pmu, i);
        printf("[test] Worker %d miss rate: %.2f misses/ms\n", i, rate);
    }
}
    
    // sStop the background thread
    pmu_stop(&pmu);
    
    return 0;
}