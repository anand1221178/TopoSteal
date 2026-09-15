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

/* Urgency-aware victim selection (step 3): blends the existing
 * topology(+PMU-feedback) weighting in w->steal_thresholds with each
 * candidate's predicted "richness" - richness_ns[j], nanoseconds of ready
 * work at worker j (queue_depth * avg_task_duration), supplied by the
 * caller. A topologically-close worker with a drained queue no longer
 * automatically outranks a farther one that actually has fast, ready
 * work. richness_ns[j] <= 0 ("unknown yet") is treated as neutral rather
 * than zeroed out, so a worker we haven't learned about yet isn't
 * permanently starved of steal attempts. Composes on top of whatever
 * weights_init/feedback_update last computed rather than replacing it -
 * distance and PMU feedback stay exactly as before when richness is
 * uniform across workers (reduces to weights_pick_victim in that case). */
int weights_pick_victim_urgent(weights_t *w, int worker_id, const double *richness_ns, unsigned int *seed);

#endif