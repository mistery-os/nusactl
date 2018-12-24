#include <sched.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <nusa.h>

int main(int argc, char *argv[])
{
	nodemask_t nodemask;
	int rc, i;

	rc = nusa_available();
	printf("nusa_available returns %d\n", rc);
	if (rc < 0) exit(1);

	nodemask_zero(&nodemask);

	nodemask = nusa_get_run_node_mask();
	for (i = 0; i < 4; i++) {
		printf("nusa_get_run_node_mask nodemask_isset returns=0x%lx\n", nodemask_isset(&nodemask, i));
	}

	rc = nusa_run_on_node_mask(&nodemask);
	printf("rc=%d from nusa_run_on_node_mask\n", rc);

	return (0);
}
