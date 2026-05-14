#ifndef FEEDBACK_H
#define FEEDBACK_H

#include "topo.h"
#include "pmu.h"
#include "weights.h"

typedef struct 
{
    topo_t *t; //Topology
    weights_t * w; //wwights
    pmu_t *p; //pmu
}feedback_t;


/* iNitn */
void feedback_init(feedback_t *f, topo_t *t, weights_t *w, pmu_t *p);


//Feedback loop
void feedback_update(feedback_t *f);

#endif