#define _GNU_SOURCE
#include <stdio.h>
#include <stdatomic.h>
#include "../include/toposteal.h"

static _Atomic int counter = 0;

static void increment(void *arg) {
    (void)arg;
    atomic_fetch_add(&counter, 1);
}

int main() {
    toposteal_t *ts = toposteal_init(4);

    for (int i = 0; i < 1000; i++)
        toposteal_submit(ts, increment, NULL);

    toposteal_wait(ts);

    printf("Counter: %d / 1000 — %s\n",
        atomic_load(&counter),
        atomic_load(&counter) == 1000 ? "PASSED" : "FAILED");

    toposteal_destroy(ts);
    return 0;
}