/* Test wrapper for the nodemask parser */
#include <stdio.h>
#include "nusa.h"
#include "util.h"

/* For util.c. Fixme. */
void usage(void)
{
	exit(1);
}

int main(int ac, char **av)
{
	int err = 0;
	while (*++av) {
		struct bitmask *mask = nusa_parse_nodestring(*av);
		if (!mask) {
			printf("Failed to convert `%s'\n", *av);
			err |= 1;
			continue;
		}
		printmask("result", mask);
		nusa_bitmask_free(mask);
	}
	return err;
}
