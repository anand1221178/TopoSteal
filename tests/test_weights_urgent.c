#define _GNU_SOURCE
#include <stdio.h>
#include <time.h>
#include "../include/topo.h"
#include "../include/weights.h"

#define TRIALS 20000

static int trial_pick(weights_t *w, int worker_id, const double *richness, unsigned int *seed) {
    return weights_pick_victim_urgent(w, worker_id, richness, seed);
}

int main() {
    topo_t t;
    weights_t w;
    topo_init(&t);
    weights_init(&w, &t);

    unsigned int seed = (unsigned int)time(NULL);

    /* --- Case 1: uniform richness should reduce to plain topology
     * weighting (weights_pick_victim) - same distribution, since the
     * richness factor is identical (=1) for every candidate. --- */
    double uniform[TOPO_MAX_CORES];
    for (int i = 0; i < t.num_cores; i++) uniform[i] = 100.0; /* same for all */

    int counts_plain[TOPO_MAX_CORES] = {0};
    int counts_uniform[TOPO_MAX_CORES] = {0};
    for (int i = 0; i < TRIALS; i++) {
        int a = weights_pick_victim(&w, 0, &seed);
        int b = trial_pick(&w, 0, uniform, &seed);
        if (a >= 0) counts_plain[a]++;
        if (b >= 0) counts_uniform[b]++;
    }

    printf("--- Case 1: uniform richness should match plain topology weighting ---\n");
    int case1_ok = 1;
    for (int j = 1; j < t.num_cores; j++) {
        double p_plain = counts_plain[j] / (double)TRIALS * 100.0;
        double p_uniform = counts_uniform[j] / (double)TRIALS * 100.0;
        double diff = p_plain - p_uniform;
        if (diff < 0) diff = -diff;
        printf("  Worker %2d: plain=%.1f%%  urgent(uniform)=%.1f%%  diff=%.1f%%\n",
               j, p_plain, p_uniform, diff);
        if (diff > 2.0) case1_ok = 0; /* small tolerance for RNG noise */
    }
    printf("  %s\n\n", case1_ok ? "PASSED" : "FAILED");

    /* --- Case 2: a far-but-rich victim should be able to outrank a
     * near-but-poor one. Worker 0's near same-cluster peers (dist=1) get
     * near-zero richness; one far worker (different cluster, dist=8) gets
     * very high richness. Everyone else is zeroed out so they don't
     * dilute the comparison. --- */
    int near_worker = -1, far_worker = -1;
    for (int j = 1; j < t.num_cores; j++) {
        if (topo_get_distance(&t, 0, j) == TOPO_DIST_SAME) continue;
        if (near_worker < 0 && topo_get_distance(&t, 0, j) <= TOPO_DIST_L2_SHARED) near_worker = j;
        if (topo_get_distance(&t, 0, j) >= TOPO_DIST_PACKAGE) far_worker = j;
    }

    if (near_worker < 0 || far_worker < 0) {
        printf("--- Case 2: SKIPPED (topology doesn't have both a near and a far candidate) ---\n");
        topo_destroy();
        return case1_ok ? 0 : 1;
    }

    double skewed[TOPO_MAX_CORES];
    for (int j = 0; j < t.num_cores; j++) skewed[j] = 0.0; /* neutral/unknown for everyone else */
    skewed[near_worker] = 1.0;      /* near, but almost drained */
    skewed[far_worker]  = 5000.0;   /* far, but deep ready queue */

    int counts_skewed[TOPO_MAX_CORES] = {0};
    int total_to_pair = 0;
    for (int i = 0; i < TRIALS; i++) {
        int v = trial_pick(&w, 0, skewed, &seed);
        if (v == near_worker || v == far_worker) {
            counts_skewed[v]++;
            total_to_pair++;
        }
    }

    double near_dist = topo_get_distance(&t, 0, near_worker);
    double far_dist = topo_get_distance(&t, 0, far_worker);
    double near_pct = total_to_pair ? counts_skewed[near_worker] / (double)total_to_pair * 100.0 : 0.0;
    double far_pct = total_to_pair ? counts_skewed[far_worker] / (double)total_to_pair * 100.0 : 0.0;

    printf("--- Case 2: far-but-rich should outrank near-but-poor ---\n");
    printf("  near worker %d (dist=%.0f, richness=1):    %.1f%% of steals-to-this-pair\n",
           near_worker, near_dist, near_pct);
    printf("  far  worker %d (dist=%.0f, richness=5000): %.1f%% of steals-to-this-pair\n",
           far_worker, far_dist, far_pct);
    int case2_ok = far_pct > near_pct;
    printf("  %s\n\n", case2_ok ? "PASSED" : "FAILED");

    topo_destroy();
    printf(case1_ok && case2_ok ? "ALL PASSED\n" : "SOME FAILED\n");
    return (case1_ok && case2_ok) ? 0 : 1;
}
