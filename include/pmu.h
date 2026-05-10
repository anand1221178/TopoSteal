#ifndef PMU_H
#define PMU_H
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include "topo.h"
typedef struct
{
    /* NUmber of workers */
    uint32_t num_workers;
        /* File descriptors - one for each works perf_event_open*/
    int fds[TOPO_MAX_CORES];
        /* Previous readings - for delta calculations */
    uint64_t previous_readings[TOPO_MAX_CORES];
        /* current miss rates - also for calcs */
    _Atomic float miss_rates[TOPO_MAX_CORES];
        /* background thread handle */
    pthread_t sampler_thread;
        /* switch for turning off the thread */
    _Atomic int keep_running;
    /* 1 = hardware counters, 0 = software proxy */
    int hardware_mode;
}pmu_t;

/* Function declares */
/* init */
int pmu_init(pmu_t *pmu, uint32_t num_workers);
/* start pmu */
void pmu_start(pmu_t *pmu);
/* Stop pmu */
void pmu_stop(pmu_t *pmu);
/* miss rate */
float pmu_get_miss_rate(pmu_t *pmu, uint32_t worker_id);
#endif