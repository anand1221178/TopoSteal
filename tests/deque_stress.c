#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>
#include "../include/deque.h"

#define NUM_WORKERS 4
#define DURATION_SEC 10

static deque_t queues[NUM_WORKERS];
static _Atomic int tasks_completed = 0;
static _Atomic int tasks_submitted = 0;
static _Atomic int steal_collisions = 0;
static _Atomic int running = 1;

static void dummy_task(void *arg) {
    (void)arg;
    atomic_fetch_add(&tasks_completed, 1);
}

static void *worker(void *arg) {
    int id = *(int *)arg;
    unsigned int seed = (unsigned int)time(NULL) ^ id;
    task_t task;

    while (atomic_load(&running)) {
        task_t t = { .fn = dummy_task, .arg = NULL };
        deque_push(&queues[id], t);
        atomic_fetch_add(&tasks_submitted, 1);

        if (deque_pop(&queues[id], &task)) {
            task.fn(task.arg);
        }

        int victim = rand_r(&seed) % NUM_WORKERS;
        if (victim != id) {
            if (deque_steal(&queues[victim], &task)) {
                task.fn(task.arg);
            } else {
                atomic_fetch_add(&steal_collisions, 1);
            }
        }
    }

    task_t drain;
    while (deque_pop(&queues[id], &drain))
        drain.fn(drain.arg);

    return NULL;
}

int main() {
    printf("[stress] %d workers, %ds duration\n", NUM_WORKERS, DURATION_SEC);

    for (int i = 0; i < NUM_WORKERS; i++)
        deque_init(&queues[i]);

    int ids[NUM_WORKERS];
    pthread_t threads[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; i++) {
        ids[i] = i;
        pthread_create(&threads[i], NULL, worker, &ids[i]);
    }

    sleep(DURATION_SEC);
    atomic_store(&running, 0);

    for (int i = 0; i < NUM_WORKERS; i++)
        pthread_join(threads[i], NULL);

    int submitted = atomic_load(&tasks_submitted);
    int completed = atomic_load(&tasks_completed);
    int collisions = atomic_load(&steal_collisions);

    printf("[stress] tasks submitted:   %d\n", submitted);
    printf("[stress] tasks completed:   %d\n", completed);
    printf("[stress] tasks lost:        %d\n", submitted - completed);
    printf("[stress] steal collisions:  %d\n", collisions);
    printf("[stress] %s\n", (submitted == completed) ? "PASSED" : "FAILED");

    return (submitted == completed) ? 0 : 1;
}
