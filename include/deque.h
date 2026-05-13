#ifndef DEQUE_H
#define DEQUE_H

#define DEQUE_MAX_TASKS 1024

#include <stdatomic.h>
#include <stddef.h>
#include "pmu.h"

/* Task struct */
typedef struct
{
    void (*fn)(void *);
    void *arg;
}task_t;

/* Deque strcuture */
typedef struct
{
    _Atomic size_t top;
    _Atomic size_t bottom;
    task_t tasks[DEQUE_MAX_TASKS]; /* We are keeping it constant since the number of workers is constant */
}deque_t;

/* Function declares */
void deque_init(deque_t *q);

void deque_push(deque_t *q, task_t t);

/* Returns 1 if success, else 0 */
int deque_pop(deque_t *q, task_t *t_out);

/* Returns 1 if success, else 0 */
int deque_steal(deque_t *q, task_t *t_out);

#endif