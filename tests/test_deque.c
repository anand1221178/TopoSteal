#define _GNU_SOURCE
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include "../include/deque.h"

#define NUM_TASKS 100

static deque_t q;
static _Atomic int completed = 0;

static void dummy_task(void *arg) {
    (void)arg;
    atomic_fetch_add(&completed, 1);
}

static void *thief_thread(void *arg) {
    (void)arg;
    task_t t;
    int stolen = 0;
    while (atomic_load(&completed) < NUM_TASKS) {
        if (deque_steal(&q, &t)) {
            t.fn(t.arg);
            stolen++;
        }
    }
    printf("[thief] stole and executed %d tasks\n", stolen);
    return NULL;
}

int main() {
    deque_init(&q);

    // Launch thief thread
    pthread_t thief;
    pthread_create(&thief, NULL, thief_thread, NULL);

    // Owner pushes and pops
    int popped = 0;
    for (int i = 0; i < NUM_TASKS; i++) {
        task_t t = { .fn = dummy_task, .arg = NULL };
        deque_push(&q, t);
    }

    task_t t;
    while (deque_pop(&q, &t)) {
        t.fn(t.arg);
        popped++;
    }
    printf("[owner] popped and executed %d tasks\n", popped);

    pthread_join(thief, NULL);

    printf("\n--- Result ---\n");
    printf("Tasks submitted: %d\n", NUM_TASKS);
    printf("Tasks completed: %d\n", atomic_load(&completed));
    if (atomic_load(&completed) == NUM_TASKS)
        printf("PASSED - zero task loss\n");
    else
        printf("FAILED - %d tasks lost!\n", NUM_TASKS - atomic_load(&completed));

    return 0;
}