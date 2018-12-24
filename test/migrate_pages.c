/*
 * Test program to test the moving of a processes pages.
 *
 * (C) 2006 Silicon Graphics, Inc.
 *		Christoph Lameter <clameter@sgi.com>
 */
#include <stdio.h>
#include <stdlib.h>
#include <nusa.h>
#include <unistd.h>
#include <errno.h>

unsigned int pagesize;
unsigned int page_count = 32;

char *page_base;
char *pages;

void **addr;
int *status;
int *nodes;
int errors;
int nr_nodes;

struct bitmask *old_nodes;
struct bitmask *new_nodes;

int main(int argc, char **argv)
{
	int i, rc;

	pagesize = getpagesize();

	nr_nodes = nusa_max_node()+1;

	old_nodes = nusa_bitmask_alloc(nr_nodes);
        new_nodes = nusa_bitmask_alloc(nr_nodes);
        nusa_bitmask_setbit(old_nodes, 1);
        nusa_bitmask_setbit(new_nodes, 0);

	if (nr_nodes < 2) {
		printf("A minimum of 2 nodes is required for this test.\n");
		exit(1);
	}

	setbuf(stdout, NULL);
	printf("migrate_pages() test ......\n");
	if (argc > 1)
		sscanf(argv[1], "%d", &page_count);

	page_base = malloc((pagesize + 1) * page_count);
	addr = malloc(sizeof(char *) * page_count);
	status = malloc(sizeof(int *) * page_count);
	nodes = malloc(sizeof(int *) * page_count);
	if (!page_base || !addr || !status || !nodes) {
		printf("Unable to allocate memory\n");
		exit(1);
	}

	pages = (void *) ((((long)page_base) & ~((long)(pagesize - 1))) + pagesize);

	for (i = 0; i < page_count; i++) {
		if (i != 2)
			/* We leave page 2 unallocated */
			pages[ i * pagesize ] = (char) i;
		addr[i] = pages + i * pagesize;
		nodes[i] = 1;
		status[i] = -123;
	}

	/* Move to starting node */
	rc = nusa_move_pages(0, page_count, addr, nodes, status, 0);
	if (rc < 0 && errno != ENOENT) {
		perror("move_pages");
		exit(1);
	}

	/* Verify correct startup locations */
	printf("Page location at the beginning of the test\n");
	printf("------------------------------------------\n");

	nusa_move_pages(0, page_count, addr, NULL, status, 0);
	for (i = 0; i < page_count; i++) {
		printf("Page %d vaddr=%p node=%d\n", i, pages + i * pagesize, status[i]);
		if (i != 2 && status[i] != 1) {
			printf("Bad page state before migrate_pages. Page %d status %d\n",i, status[i]);
			exit(1);
		}
	}

	/* Move to node zero */
	nusa_move_pages(0, page_count, addr, nodes, status, 0);

	printf("\nMigrating the current processes pages ...\n");
	rc = nusa_migrate_pages(0, old_nodes, new_nodes);

	if (rc < 0) {
		perror("nusa_migrate_pages failed");
		errors++;
	}

	/* Get page state after migration */
	nusa_move_pages(0, page_count, addr, NULL, status, 0);
	for (i = 0; i < page_count; i++) {
		printf("Page %d vaddr=%lx node=%d\n", i,
			(unsigned long)(pages + i * pagesize), status[i]);
		if (i != 2) {
			if (pages[ i* pagesize ] != (char) i) {
				printf("*** Page contents corrupted.\n");
				errors++;
			} else if (status[i]) {
				printf("*** Page on the wrong node\n");
				errors++;
			}
		}
	}

	if (!errors)
		printf("Test successful.\n");
	else
		printf("%d errors.\n", errors);

	return errors > 0 ? 1 : 0;
}
