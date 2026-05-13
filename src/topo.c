#include "topo.h"
#include <hwloc.h>
#include <stdio.h>
#include <string.h>


//Topo.c only this file can see this
static hwloc_topology_t hwloc_topo;

// When hwloc cant detect real cache topology (Lima / Apple Silicon)
static void topo_apply_mock(topo_t *t);

void topo_init(topo_t *t)
{
	/*Clear the memory since we dont want junk allocs*/
	memset(t, 0, sizeof(*t));

	/* initilaise hwloc -> opaque handle*/
	hwloc_topology_init(&hwloc_topo); /*init will allocate the handle*/
	hwloc_topology_load(hwloc_topo); /*load will read the HW*/

	/*We need to now find the number of cores*/
	//Hwloc represents everything as objects in a tree, so we ask hwloc for the depth of the core level in the tree, and then count how many objects exisit in that depth.
	
	int depth = hwloc_get_type_depth(hwloc_topo, HWLOC_OBJ_CORE);
	if (depth == HWLOC_TYPE_DEPTH_UNKNOWN) {
    		printf("[topo] WARNING: core depth unknown, applying mock topology\n");
    		t->num_cores = 4;  // default
    		topo_apply_mock(t);
    		return;            // early exit, skip the hwloc tree walk
	}

	t->num_cores = hwloc_get_nbobjs_by_depth(hwloc_topo, depth);
	if (t->num_cores > TOPO_MAX_CORES) t->num_cores = TOPO_MAX_CORES;

	/*Flag to catch junk hwloc calls*/
	int mock_needed = 0;


	/*NOw we walk the ancestors in the loop*/
	printf("[topo] SETUP: walking hwloc tree and gathering ancestors\n");
	for(int i =0; i < t->num_cores; ++i)
	{
		for(int j = 0; j < t->num_cores; j++)
		{
			/*Casae that they are the same*/
			if (i == j) {
   			 	t->distance[i][j] = TOPO_DIST_SAME;
			 	continue;
			}

			/*Get the hwloc object for core i*/
			hwloc_obj_t ci = hwloc_get_obj_by_depth(hwloc_topo, depth, i);

			/*Get the hwloc object for core j*/
			hwloc_obj_t cj = hwloc_get_obj_by_depth(hwloc_topo, depth, j);

			/*Find common ancestor*/
			hwloc_obj_t ancestor = hwloc_get_common_ancestor_obj(hwloc_topo, ci, cj);

			/*Safety check if ancestor is NULL -> assign to worst case since no ancestor*/
			if(ancestor == NULL) 
			{
				t->distance[i][j] = TOPO_DIST_NUMA;
				continue;
			}

			/*Case switch statement*/
			switch(ancestor->type)
			{
				case HWLOC_OBJ_L2CACHE:
					t->distance[i][j] = TOPO_DIST_L2_SHARED;
					break;
				case HWLOC_OBJ_L3CACHE:
					t->distance[i][j] = TOPO_DIST_L3_SHARED;
					break;
				case HWLOC_OBJ_PACKAGE:
					t->distance[i][j] = TOPO_DIST_PACKAGE;
					break;
				case HWLOC_OBJ_NUMANODE:
					t->distance[i][j] = TOPO_DIST_NUMA;
					break;
				default :
					/*FALBACK for any erros in hwloc due to wierdness*/
					mock_needed =1;
					t->distance[i][j] = TOPO_DIST_PACKAGE;
					break;

			}
		}
	}
	/* If we hit weird hypervisor objects, our matrix is junk. Overwrite it. */
    	if (mock_needed == 1) {
        	topo_apply_mock(t);
    	}

}

void topo_apply_mock(topo_t *t)
{
	/**Warn user that we are using mock topology*/
	printf("[topo] WARNING: using a mock topology!\n");
	for(int i = 0; i < t->num_cores; i++)
	{
		for(int j = 0; j < t->num_cores; j++)
		{
			if(i==j) /*TOPO distance is the same*/
			{
				t->distance[i][j] = TOPO_DIST_SAME;
			}
			else if((i/2)==(j/2))
			{
				t->distance[i][j] = TOPO_DIST_L2_SHARED;
			}
			else
			{
				t->distance[i][j] = TOPO_DIST_PACKAGE;
			}
		}
	}

}

void topo_destroy(void)
{
	hwloc_topology_destroy(hwloc_topo);
}


void topo_print(topo_t *t)
{
	printf("\nDistance matrix (%d cores):\n     ", t->num_cores);
	
	/* Print the column headers once at the top */
	for (int i = 0; i < t->num_cores; i++) {
		printf("C%-3d ", i);
	}
	printf("\n");
	
	/* Print the rows */
	for(int i = 0; i < t->num_cores; i++) // rows
	{
		/* Print the row header */
		printf("C%-3d ", i);
		
		for(int j = 0; j < t->num_cores; j++) // cols
		{
			/* Print just the distance value */
			printf("%-4d ", t->distance[i][j]);
		}
		/* Hit Enter at the end of the row */
		printf("\n");
	}
}

uint32_t topo_get_distance(topo_t *t, uint8_t index_1, uint8_t index_2)
{
	return t->distance[index_1][index_2];
}