#ifndef WEIGHTS_H
#define WEIGHTS_H

#include "topo.h"
#include "deque.h"
#include <stdlib.h>


typedef struct 
{
    /* 2D array, one row per works, one col per potential victim*/
    float steal_thresholds[TOPO_MAX_CORES][TOPO_MAX_CORES];

    int num_workers;
}weights_t;

void weights_init(weights_t *w, topo_t *t);

int weights_pick_victim(weights_t *w, int worker_id, unsigned int *seed);

#endif