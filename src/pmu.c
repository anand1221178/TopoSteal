#include "pmu.h" /* pmu struct */
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>


int pmu_init(pmu_t *pmu, uint32_t num_workers, int *cpu_map)
{
    /* Clear struct with memset */
    memset(pmu, 0, sizeof(*pmu));
    pmu->num_workers = num_workers;

    /* Create perf event stucrt */
    struct perf_event_attr attr;

    memset(&attr, 0, sizeof(attr));

    /* Define attr for perf event*/
    attr.size = sizeof(attr);
    attr.type = PERF_TYPE_HARDWARE;
    attr.config = PERF_COUNT_HW_CACHE_MISSES;
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;

    for (int i =0 ; i< num_workers; i ++)
    {
        int cpu = cpu_map ? cpu_map[i] : i;
        int fd = syscall(SYS_perf_event_open, &attr, 0, cpu, -1, 0);
        
        /* Check for failure */
        if (fd == -1)
        {
            /* Set to software mode mode */
            pmu->hardware_mode = 0;
            attr.type = PERF_TYPE_SOFTWARE;
            attr.config = PERF_COUNT_SW_PAGE_FAULTS;
            /* Retry with software mode */
            /* If still fails then total failure */
            int fd_retry = syscall(SYS_perf_event_open, &attr, 0, cpu, -1, 0);
            if (fd_retry == -1)
            {
                printf("[TOPOSTEAL] Warning file descriptors failed to launch!");
                return -1;
            }
            else{
                pmu->hardware_mode = 0;
                pmu->fds[i] = fd_retry;
                pmu->previous_readings[i] = 0;
            }
        }
        else{
            pmu->hardware_mode = 1;
            pmu->fds[i] = fd;
            pmu->previous_readings[i] = 0;
        }
    }

    return 0; /* Success for the init */
}

/* Sampler thread -> background loop kthat reads counters every 10ms */
static void *pmu_sampler_thread(void *arg)
{
    /* cast from void to pmu */
    pmu_t *pmu = (pmu_t *)arg;

    int tick = 0;
    while(pmu->keep_running == 1)
    {
        /* For each worker */
        for (int i = 0; i < pmu->num_workers; ++i)
        {
            uint64_t curr_count;
            uint64_t delta;
            read(pmu->fds[i], &curr_count, sizeof(curr_count));
            delta = curr_count - pmu->previous_readings[i];
            pmu->miss_rates[i] = (float)delta /10.0f;
            pmu->previous_readings[i] = curr_count;
        }

        tick++;
        if (tick % 5 == 0 && pmu->feedback_cb)
            pmu->feedback_cb(pmu->feedback_ctx);

        usleep(10000);
    }

    return NULL;
}

void pmu_start(pmu_t *pmu)
{
    /* Tell thread it is allowed to run*/
    pmu->keep_running = 1;
    /* Reset and enable all h/s counters */
    for (int i = 0; i < pmu->num_workers; i++)
    {
            ioctl(pmu->fds[i], PERF_EVENT_IOC_RESET, 0);
            ioctl(pmu->fds[i], PERF_EVENT_IOC_ENABLE, 0);
    }

    /* aluanch background thread */
    pthread_create(&pmu->sampler_thread, NULL, pmu_sampler_thread, pmu);
    printf("[TOPOSTEAL] PMU Sampler Thread started.\n");
}

void pmu_stop(pmu_t *pmu)
{
    pmu->keep_running = 0;
    pthread_join(pmu->sampler_thread, NULL);

    for (int i = 0; i < pmu->num_workers; i++)
    {
            ioctl(pmu->fds[i], PERF_EVENT_IOC_DISABLE, 0);
            close(pmu->fds[i]);
    }
    printf("[TOPOSTEAL] PMU Sampler Thread stopped.\n");
}

float pmu_get_miss_rate(pmu_t *pmu, uint32_t worker_id)
{
    /* BOunds check! */
    if(worker_id < pmu->num_workers)
    {
        return pmu->miss_rates[worker_id];
    }
    else{
        return -1;
    }
}