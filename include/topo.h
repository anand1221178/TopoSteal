#ifndef TOPO_H
#define TOPO_H

#include <stdint.h>

#define TOPO_MAX_CORES 128

#define TOPO_DIST_SAME       0
#define TOPO_DIST_CORE       1
#define TOPO_DIST_L2_SHARED  1
#define TOPO_DIST_L3_SHARED  4
#define TOPO_DIST_PACKAGE    8
#define TOPO_DIST_NUMA       10

typedef struct
{
	int num_cores;
	int distance[TOPO_MAX_CORES][TOPO_MAX_CORES];
	int cpu_map[TOPO_MAX_CORES];
}topo_t;

void topo_init(topo_t *t);
void topo_print(topo_t *t);
uint32_t topo_get_distance(topo_t *t, uint8_t index_1, uint8_t index_2);
void topo_destroy(void);


#endif
