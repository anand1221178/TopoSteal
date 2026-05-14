#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../include/weights.h"
#include "../include/topo.h"

int main() {
    topo_t t;
    weights_t w;

    topo_init(&t);
    topo_print(&t);
    weights_init(&w, &t);

    // Count how often each victim is chosen from worker 0
    int counts[TOPO_MAX_CORES] = {0};
    unsigned int seed = (unsigned int)time(NULL);
    int trials = 10000;

    for (int i = 0; i < trials; i++) {
        int victim = weights_pick_victim(&w, 0, &seed);
        if (victim >= 0) counts[victim]++;
    }

    printf("\n--- Steal distribution from Worker 0 (%d trials) ---\n", trials);
    for (int i = 0; i < t.num_cores; i++) {
        if (i == 0) { printf("  Worker %d (self):   skipped\n", i); continue; }
        printf("  Worker %d (dist=%d): %d steals (%.1f%%)\n",
            i,
            topo_get_distance(&t, 0, i),
            counts[i],
            (float)counts[i] / trials * 100.0f);
    }

    topo_destroy();
    return 0;
}