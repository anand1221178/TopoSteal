#include "weights.h"

void weights_init(weights_t *w, topo_t *t)
{
    /* Pass 1 to get the total and setup */

    w->num_workers = t->num_cores;

    for(int i =0; i < t->num_cores; ++i)
    {
        float row_sum = 0.f;
        for(int j =0 ; j < t->num_cores; ++j)
        {
            if (i != j)
            {
                int dist = topo_get_distance(t, i, j);
                if (dist == 0) continue;
                row_sum += 1.0f / ((float)dist * dist);
            }
        }

        float accumalator = 0.f;
        for(int j =0 ; j < t->num_cores; ++j)
        {
            if(i == j)
            {
                w->steal_thresholds[i][j] = 0.0;
                continue;
            }
            int dist = topo_get_distance(t,i,j);
            
            float victim_piece = (1.0f / ((float)dist * dist)) / row_sum;

            accumalator += victim_piece;
            w->steal_thresholds[i][j] = accumalator;
        }
    }
}

int weights_pick_victim(weights_t *w, int worker_id, unsigned int *seed)
{
    /* Generate random number since we now have to picjk based on probabilities - we cannot use rand() since it is not thread safe */
    float roll = (float)rand_r(seed) / (float)RAND_MAX;


    /* Walk through the victims of worker_id */
    for(int i = 0; i < w->num_workers; i++)
    {
        if (roll <= w->steal_thresholds[worker_id][i])
        {
            return i;
        }
    }


    /* Failure case */
    return -1;

}

int weights_pick_victim_urgent(weights_t *w, int worker_id, const double *richness_ns, unsigned int *seed)
{
    int n = w->num_workers;
    float slice[TOPO_MAX_CORES];
    float total = 0.0f;

    /* Normalize richness against the max seen, same style as feedback.c's
     * miss-rate normalisation - keeps the scale-free (richness is in
     * nanoseconds, which would otherwise swamp the 1/dist^2 weights). */
    double max_richness = 0.0;
    for (int j = 0; j < n; j++)
        if (richness_ns[j] > max_richness) max_richness = richness_ns[j];

    /* Recover each victim's raw (non-cumulative) topology(+feedback)
     * weight from the existing cumulative thresholds, then scale it by
     * relative richness. This composes with whatever weights_init/
     * feedback_update already computed rather than replacing it. */
    float prev = 0.0f;
    for (int j = 0; j < n; j++) {
        float cum = w->steal_thresholds[worker_id][j];
        float base = (j == worker_id) ? 0.0f : (cum - prev);
        if (base < 0.0f) base = 0.0f; /* guard against float noise */
        prev = cum;

        double factor = (max_richness > 0.0 && richness_ns[j] > 0.0)
                       ? (richness_ns[j] / max_richness)
                       : 1.0; /* unknown/idle-looking richness -> neutral, not zeroed */

        slice[j] = base * (float)factor;
        total += slice[j];
    }

    if (total <= 0.0f) return -1;

    float roll = ((float)rand_r(seed) / (float)RAND_MAX) * total;
    float acc = 0.0f;
    for (int j = 0; j < n; j++) {
        acc += slice[j];
        if (roll <= acc) return j;
    }

    return -1;
}