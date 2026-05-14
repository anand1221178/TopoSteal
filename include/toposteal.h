#ifndef TOPOSTEAL_H
#define TOPOSTEAL_H

#include "feedback.h"
#include <stdatomic.h>

typedef struct toposteal_t toposteal_t;

/* Worker context struct */


toposteal_t*  toposteal_init(int num_workers);

void toposteal_submit(toposteal_t *ts, void (*fn)(void *), void *arg);

void toposteal_wait(toposteal_t *ts);

void toposteal_destroy(toposteal_t *ts);

#endif