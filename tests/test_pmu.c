#include <stdio.h>
#include <unistd.h>
#include "../include/pmu.h"

int main() {
    pmu_t pmu;
    
    //Init with 4 workers
    if (pmu_init(&pmu, 4) == -1) {
        printf("[test] PMU failed to initialize. Exiting.\n");
        return -1;
    }
    
    // tart the background thread
    pmu_start(&pmu);
    
    /* Sample for 500 ms */
    usleep(500000);
    
    // print miss rates for all 4 workers
    printf("\n--- PMU Readings ---\n");
    for (int i = 0; i < 4; i++) {
        float rate = pmu_get_miss_rate(&pmu, i);
        printf("[test] Worker %d miss rate: %.2f\n", i, rate);
    }
    printf("--------------------\n\n");
    
    // sStop the background thread
    pmu_stop(&pmu);
    
    return 0;
}