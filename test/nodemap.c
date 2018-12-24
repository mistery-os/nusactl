#include "nusa.h"
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
	int i, k, w, ncpus;
	struct bitmask *cpus;
	int maxnode = nusa_num_configured_nodes()-1;

	if (nusa_available() < 0)  {
		printf("no nusa\n");
		exit(1);
	}
	cpus = nusa_allocate_cpumask();
	ncpus = cpus->size;

	for (i = 0; i <= maxnode ; i++) {
		if (nusa_node_to_cpus(i, cpus) < 0) {
			printf("node %d failed to convert\n",i);
		}
		printf("%d: ", i);
		w = 0;
		for (k = 0; k < ncpus; k++)
			if (nusa_bitmask_isbitset(cpus, k))
				printf(" %s%d", w>0?",":"", k);
		putchar('\n');
	}
	return 0;
}
