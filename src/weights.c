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
                row_sum += (1.0f/dist);
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
            
            float victim_piece = (1.0f/dist)/row_sum;

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