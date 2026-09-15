#ifndef TOPOSTEAL_H
#define TOPOSTEAL_H

#include "feedback.h"
#include <stdatomic.h>

typedef struct toposteal_t toposteal_t;

/* Scheduling policy, for the required 3-way ablation (see the plan):
 * TOPOSTEAL_POLICY_TOPOLOGY_REACTIVE - original TopoSteal: reactive only
 *   (no steal-ahead), victim picked by topology(+PMU feedback) alone.
 * TOPOSTEAL_POLICY_PREDICTION_ONLY   - A2WS-shaped: preemptive steal-ahead
 *   enabled, but victim picked uniformly at random (topology-agnostic).
 * TOPOSTEAL_POLICY_COMBINED          - this project's contribution:
 *   preemptive steal-ahead + topology-and-richness-aware victim choice.
 */
#define TOPOSTEAL_POLICY_TOPOLOGY_REACTIVE 0
#define TOPOSTEAL_POLICY_PREDICTION_ONLY   1
#define TOPOSTEAL_POLICY_COMBINED          2

toposteal_t*  toposteal_init(int num_workers); /* = toposteal_init_policy(num_workers, TOPOSTEAL_POLICY_COMBINED) */
toposteal_t*  toposteal_init_policy(int num_workers, int policy);

void toposteal_submit(toposteal_t *ts, void (*fn)(void *), void *arg);

void toposteal_wait(toposteal_t *ts);

void toposteal_destroy(toposteal_t *ts);

/* Prints P50/P99/P99.9/max task completion latency (enqueue -> finished
 * executing) to stdout, in ms. Samples are dropped past a fixed capacity
 * (see TOPOSTEAL_MAX_LATENCY_SAMPLES in toposteal.c) - fine for percentile
 * estimation, not an exhaustive log. */
void toposteal_print_latency_stats(toposteal_t *ts);

#endif