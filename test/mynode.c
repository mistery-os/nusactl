#include <nusa.h>
#include <nusaif.h>
#include <stdio.h>

int main(void)
{
	int nd;
	char *man = nusa_alloc(1000);
	*man = 1;
	if (get_prampolicy(&nd, NULL, 0, man, MPOL_F_NODE|MPOL_F_ADDR) < 0)
		perror("get_prampolicy");
	else
		printf("my node %d\n", nd);
	return 0;
}
