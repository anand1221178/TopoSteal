#include "feedback.h"

void feedback_init(feedback_t *f, topo_t *t, weights_t *w, pmu_t *p)
{
    f->t = t;
    f->w = w;
    f->p = p;
}

void feedback_update(feedback_t *f)
{
    /* Find max miss rate accross all workers - for normalisation */
    float max_miss = 0.f;
    for (int i = 0; i < f->t->num_cores; i++)
    {
        float rate = pmu_get_miss_rate(f->p, i);
        if (rate > max_miss) {
            max_miss = rate;
        }
    }

    /* If no one has cache misse dont change*/
    if (max_miss == 0.0f) {
        return; 
    }

    /* Recompute for each pair i,j effective weight */
    /* Rebuild cumalative threshold table */

    for (int i = 0; i < f->t->num_cores; i++)
    {
        /* PASS 1 Get the new total sum */
        float row_sum = 0.0f;
        for (int j = 0; j < f->t->num_cores; j++)
        {
            if (i == j) continue; // Cant steal from yourself

            int base_dist = topo_get_distance(f->t, i, j);
            float victim_rate = pmu_get_miss_rate(f->p, j);
            float penalty = victim_rate / max_miss;
            float effective_dist = (float)base_dist * (1.0f + penalty);

            // Add the inverse of the NEW distance to the sum
            /* Gaurds */
            if (base_dist == 0) continue;
            if (effective_dist == 0.0f) continue;
            row_sum += (1.0f / (effective_dist * effective_dist));
        }

        /*PASS 2 Rebuild the cumulative threshold table*/
        float accumulator = 0.0f;
        for (int j = 0; j < f->t->num_cores; j++)
        {
            if (i == j)
            {
                f->w->steal_thresholds[i][j] = 0.0f;
                continue;
            }

            // Recalculate effective distance
            int base_dist = topo_get_distance(f->t, i, j);
            float victim_rate = pmu_get_miss_rate(f->p, j);
            float penalty = victim_rate / max_miss;
            float effective_dist = (float)base_dist * (1.0f + penalty);

            // Calculate their new slice
            float victim_piece = (1.0f / (effective_dist * effective_dist)) / row_sum;
            accumulator += victim_piece;
            f->w->steal_thresholds[i][j] = accumulator;
        }
    }
    
}