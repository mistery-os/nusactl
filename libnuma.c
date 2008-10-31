/* Simple NUMA library.
   Copyright (C) 2003,2004,2005,2008 Andi Kleen,SuSE Labs and
   Cliff Wickman,SGI.

   libnuma is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; version
   2.1.

   libnuma is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should find a copy of v2.1 of the GNU Lesser General Public License
   somewhere on your Linux system; if not, write to the Free Software 
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA 

   All calls are undefined when numa_available returns an error. */
#define _GNU_SOURCE 1
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sched.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <ctype.h>

#include <sys/mman.h>
#include <limits.h>

#include "numa.h"
#include "numaif.h"
#include "numaint.h"
#include "util.h"

#define WEAK __attribute__((weak))

#define CPU_BUFFER_SIZE 4096     /* This limits you to 32768 CPUs */

/* these are the old (version 1) masks */
nodemask_t numa_no_nodes;
nodemask_t numa_all_nodes;
/* these are now the default bitmask (pointers to) (version 2) */
struct bitmask *numa_no_nodes_ptr = NULL;
struct bitmask *numa_all_nodes_ptr = NULL;
struct bitmask *numa_all_cpus_ptr = NULL;
/* I would prefer to use symbol versioning to create v1 and v2 versions
   of numa_no_nodes and numa_all_nodes, but the loader does not correctly
   handle versioning of BSS versus small data items */

static unsigned long *node_cpu_mask_v1[NUMA_NUM_NODES];
struct bitmask **node_cpu_mask_v2;

WEAK void numa_error(char *where);

#ifdef __thread
#warning "not threadsafe"
#endif

static __thread int bind_policy = MPOL_BIND; 
static __thread unsigned int mbind_flags = 0;
static int sizes_set=0;
static int maxconfigurednode = -1;
static int maxconfiguredcpu = -1;
static int maxprocnode = -1;
static int maxproccpu = -1;
static int nodemask_sz = 0;
static int cpumask_sz = 0;

int numa_exit_on_error = 0;
int numa_exit_on_warn = 0;
static void set_sizes(void);

static inline void
nodemask_set_v1(nodemask_t *mask, int node)
{
	mask->n[node / (8*sizeof(unsigned long))] |=
		(1UL<<(node%(8*sizeof(unsigned long))));
}

static inline int
nodemask_isset_v1(const nodemask_t *mask, int node)
{
	if ((unsigned)node >= NUMA_NUM_NODES)
		return 0;
	if (mask->n[node / (8*sizeof(unsigned long))] &
		(1UL<<(node%(8*sizeof(unsigned long)))))
		return 1;
	return 0;
}

/*
 * There are two special functions, _init(void) and _fini(void), which
 * are called automatically by the dynamic loader whenever a library is loaded.
 *
 * The v1 library depends upon nodemask_t's of all nodes and no nodes.
 */
void
numa_init(void)
{
	int max,i;

	set_sizes();
	/* numa_all_nodes should represent existing nodes on this system */
        max = numa_num_configured_nodes();
        for (i = 0; i < max; i++)
                nodemask_set_v1((nodemask_t *)&numa_all_nodes, i);
	memset(&numa_no_nodes, 0, sizeof(numa_no_nodes));
}

/*
 * The following bitmask declarations, bitmask_*() routines, and associated
 * _setbit() and _getbit() routines are:
 * Copyright (c) 2004_2007 Silicon Graphics, Inc. (SGI) All rights reserved.
 * SGI publishes it under the terms of the GNU General Public License, v2,
 * as published by the Free Software Foundation.
 */
static unsigned int
_getbit(const struct bitmask *bmp, unsigned int n)
{
	if (n < bmp->size)
		return (bmp->maskp[n/bitsperlong] >> (n % bitsperlong)) & 1;
	else
		return 0;
}

static void
_setbit(struct bitmask *bmp, unsigned int n, unsigned int v)
{
	if (n < bmp->size) {
		if (v)
			bmp->maskp[n/bitsperlong] |= 1UL << (n % bitsperlong);
		else
			bmp->maskp[n/bitsperlong] &= ~(1UL << (n % bitsperlong));
	}
}

int
numa_bitmask_isbitset(const struct bitmask *bmp, unsigned int i)
{
	return _getbit(bmp, i);
}

struct bitmask *
numa_bitmask_setall(struct bitmask *bmp)
{
	unsigned int i;
	for (i = 0; i < bmp->size; i++)
		_setbit(bmp, i, 1);
	return bmp;
}

struct bitmask *
numa_bitmask_clearall(struct bitmask *bmp)
{
	unsigned int i;
	for (i = 0; i < bmp->size; i++)
		_setbit(bmp, i, 0);
	return bmp;
}

struct bitmask *
numa_bitmask_setbit(struct bitmask *bmp, unsigned int i)
{
	_setbit(bmp, i, 1);
	return bmp;
}

struct bitmask *
numa_bitmask_clearbit(struct bitmask *bmp, unsigned int i)
{
	_setbit(bmp, i, 0);
	return bmp;
}

unsigned int
numa_bitmask_nbytes(struct bitmask *bmp)
{
	return longsperbits(bmp->size) * sizeof(unsigned long);
}

/* where n is the number of bits in the map */
/* This function should not exit on failure, but right now we cannot really
   recover from this. */
struct bitmask *
numa_bitmask_alloc(unsigned int n)
{
	struct bitmask *bmp;

	if (n < 1) {
		numa_error("request to allocate mask for invalid number; abort\n");
		exit(1);
	}
	bmp = malloc(sizeof(*bmp));
	if (bmp == 0)
		goto oom;
	bmp->size = n;
	bmp->maskp = calloc(longsperbits(n), sizeof(unsigned long));
	if (bmp->maskp == 0) {
		free(bmp);
		goto oom;
	}
	return bmp;

oom:
	numa_error("Out of memory allocating bitmask");
	exit(1);
}

void
numa_bitmask_free(struct bitmask *bmp)
{
	if (bmp == 0)
		return;
	free(bmp->maskp);
	bmp->maskp = (unsigned long *)0xdeadcdef;  /* double free tripwire */
	free(bmp);
	return;
}

/* True if two bitmasks are equal */
int
numa_bitmask_equal(const struct bitmask *bmp1, const struct bitmask *bmp2)
{
	unsigned int i;
	for (i = 0; i < bmp1->size || i < bmp2->size; i++)
		if (_getbit(bmp1, i) != _getbit(bmp2, i))
			return 0;
	return 1;
}
/* *****end of bitmask_  routines ************ */

/* Next two can be overwritten by the application for different error handling */
WEAK void numa_error(char *where) 
{ 
	int olde = errno;
	perror(where); 
	if (numa_exit_on_error)
		exit(1); 
	errno = olde;
} 

WEAK void numa_warn(int num, char *fmt, ...) 
{ 
	static unsigned warned;
	va_list ap;
	int olde = errno;
	
	/* Give each warning only once */
	if ((1<<num) & warned)
		return; 
	warned |= (1<<num); 

	va_start(ap,fmt);
	fprintf(stderr, "libnuma: Warning: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);

	errno = olde;
} 

static void setpol(int policy, struct bitmask *bmp)
{ 
	if (set_mempolicy(policy, bmp->maskp, bmp->size) < 0)
		numa_error("set_mempolicy");
} 

static void getpol(int *oldpolicy, struct bitmask *bmp)
{ 
	if (get_mempolicy(oldpolicy, bmp->maskp, bmp->size, 0, 0) < 0)
		numa_error("get_mempolicy");
} 

static void dombind(void *mem, size_t size, int pol, struct bitmask *bmp)
{ 
	if (mbind(mem, size, pol, bmp ? bmp->maskp : NULL, bmp ? bmp->size : 0, 
		  mbind_flags) < 0)
		numa_error("mbind"); 
} 

/* (undocumented) */
/* gives the wrong answer for hugetlbfs mappings. */
int numa_pagesize(void)
{ 
	static int pagesize;
	if (pagesize > 0) 
		return pagesize;
	pagesize = getpagesize();
	return pagesize;
} 

make_internal_alias(numa_pagesize);

/*
 * Find the highest numbered existing memory node: maxconfigurednode.
 */
static void
set_configured_nodes(void)
{
	DIR *d;
	struct dirent *de;

	d = opendir("/sys/devices/system/node");
	if (!d) {
		numa_warn(W_nosysfs,
		   "/sys not mounted or no numa system. Assuming one node: %s",
		  	strerror(errno));
		maxconfigurednode = 0;
	} else {
		while ((de = readdir(d)) != NULL) {
			int nd;
			if (strncmp(de->d_name, "node", 4))
				continue;
			nd = strtoul(de->d_name+4, NULL, 0);
			if (maxconfigurednode < nd)
				maxconfigurednode = nd;
		}
		closedir(d);
	}
}

/*
 * Convert the string length of an ascii hex mask to the number
 * of bits represented by that mask.
 */
static int s2nbits(const char *s)
{
	return strlen(s) * 32 / 9;
}

/* Is string 'pre' a prefix of string 's'? */
static int strprefix(const char *s, const char *pre)
{
	return strncmp(s, pre, strlen(pre)) == 0;
}

static const char *mask_size_file = "/proc/self/status";
static const char *nodemask_prefix = "Mems_allowed:\t";
/*
 * (do this the way Paul Jackson's libcpuset does it)
 * The nodemask values in /proc/self/status are in an
 * ascii format that uses 9 characters for each 32 bits of mask.
 * (this could also be used to find the cpumask size)
 */
static void
set_nodemask_size(void)
{
	FILE *fp;
	char *buf = NULL;
	size_t bufsize = 0;

	if ((fp = fopen(mask_size_file, "r")) == NULL)
		goto done;

	while (getline(&buf, &bufsize, fp) > 0) {
		if (strprefix(buf, nodemask_prefix)) {
			nodemask_sz = s2nbits(buf + strlen(nodemask_prefix));
			break;
		}
	}
	free(buf);
	fclose(fp);
done:
	if (nodemask_sz == 0) {/* fall back on error */
		int pol;
		unsigned long *mask = NULL;
		nodemask_sz = 16;
		do {
			nodemask_sz <<= 1;
			mask = realloc(mask, nodemask_sz / 8);
			if (!mask)
				return;
		} while (get_mempolicy(&pol, mask, nodemask_sz + 1, 0, 0) < 0 && errno == EINVAL &&
				nodemask_sz < 4096*8);
		free(mask);
	}
}

/*
 * Read a mask consisting of a sequence of hexadecimal longs separated by
 * commas. Order them correctly and return the number of the last bit
 * set.
 */
static int
read_mask(char *s, struct bitmask *bmp)
{
	char *end = s;
	char *prevend;
	unsigned int *start = (unsigned int *)bmp->maskp;
	unsigned int *p = start;
	unsigned int *q;
	unsigned int i;
	unsigned int n = 0;

	i = strtoul(s, &end, 16);

	prevend = end;
	/* Skip leading zeros */
	while (!i && *end++ == ',') {
		prevend = end;
		i = strtoul(end, &end, 16);
	}
	end = prevend;

	if (!i)
		/* End of string. No mask */
		return -1;

	/* Read sequence of ints */
	do {
		start[n++] = i;
		i = strtoul(end, &end, 16);
	} while (*end++ == ',');
	n--;

	/*
	 * Invert sequence of ints if necessary since the first int
	 * is the highest and we put it first because we read it first.
	 */
	for (q = start + n, p = start; p < q; q--, p++) {
		unsigned int x = *q;

		*q = *p;
		*p = x;
	}

	/* Poor mans fls() */
	for(i = 31; i >= 0; i--)
		if (test_bit(i, start + n))
			break;

	/*
	 * Return the last bit set
	 */
	return ((sizeof(unsigned int)*8) * n) + i;
}

/*
 * Read a processes constraints in terms of nodes and cpus from
 * /proc/self/status.
 */
static void
set_thread_constraints(void)
{
	int hicpu = sysconf(_SC_NPROCESSORS_CONF)-1;
	int i;
	char *buffer = NULL;
	size_t buflen = 0;
	FILE *f;

	numa_all_cpus_ptr = numa_allocate_cpumask();
	numa_all_nodes_ptr = numa_allocate_nodemask();
	numa_no_nodes_ptr = numa_allocate_nodemask();

	f = fopen(mask_size_file, "r");
	if (!f) {
		//numa_warn(W_cpumap, "Cannot parse %s", mask_size_file);
		return;
	}

	while (getline(&buffer, &buflen, f) > 0) {
		if (strncmp(buffer,"Cpus_allowed:",13) == 0)
			maxproccpu = read_mask(buffer + 15, numa_all_cpus_ptr);

		if (strncmp(buffer,"Mems_allowed:",13) == 0) {
			maxprocnode =
				read_mask(buffer + 15, numa_all_nodes_ptr);
		}
	}
	fclose(f);
	free(buffer);

	/*
	 * Cpus_allowed in the kernel can be defined to all f's
	 * i.e. it may be a superset of the actual available processors.
	 * As such let's reduce maxproccpu to the number of actual
	 * available cpus - 1.
	 */
	if (maxproccpu <= 0) {
		for (i = 0; i <= hicpu; i++)
			numa_bitmask_setbit(numa_all_cpus_ptr, i);
		maxproccpu = hicpu;
	}

	if (maxproccpu > hicpu) {
		maxproccpu = hicpu;
		for (i=hicpu+1; i<numa_all_cpus_ptr->size; i++) {
			numa_bitmask_clearbit(numa_all_cpus_ptr, i);
		}
	}

	if (maxprocnode <= 0) {
		for (i = 0; i <= maxconfigurednode; i++)
			numa_bitmask_setbit(numa_all_nodes_ptr, i);
		maxprocnode = maxconfigurednode;
	}

	return;
}

/*
 * Find the highest cpu number possible (in other words the size
 * of a kernel cpumask_t (in bits) - 1)
 */
static void
set_numa_max_cpu(void)
{
	int len = 4096;
	int n;
	int olde = errno;
	struct bitmask *buffer;

	do {
		buffer = numa_bitmask_alloc(len);
		n = numa_sched_getaffinity_v2_int(0, buffer);
		/* on success, returns size of kernel cpumask_t, in bytes */
		if (n < 0 && errno == EINVAL) {
			if (len >= 1024*1024)
				break;
			len *= 2;
			numa_bitmask_free(buffer);
			continue;
		}
	} while (n < 0);
	numa_bitmask_free(buffer);
	errno = olde;
	cpumask_sz = n*8;
}

/*
 * get the total (configured) number of cpus - both online and offline
 */
static void
set_configured_cpus(void)
{
	int		filecount=0;
	char		*dirnamep = "/sys/devices/system/cpu";
	struct dirent	*dirent;
	DIR		*dir;
	dir = opendir(dirnamep);

	if (dir == NULL) {
		/* fall back to using the online cpu count */
		maxconfiguredcpu = sysconf(_SC_NPROCESSORS_CONF) - 1;
		return;
	}
	while ((dirent = readdir(dir)) != 0) {
		if (!strncmp("cpu", dirent->d_name, 3)) {
			filecount++;
		} else {
			continue;
		}
	}
	closedir(dir);
	maxconfiguredcpu = filecount-1; /* high cpu number */
	return;
}

/*
 * Initialize all the sizes.
 */
static void
set_sizes(void)
{
	sizes_set++;
	set_configured_nodes();	/* configured nodes listed in /sys */
	set_nodemask_size();	/* size of kernel nodemask_t */
	set_numa_max_cpu();	/* size of kernel cpumask_t */
	set_configured_cpus();	/* cpus listed in /sys/devices/system/cpu */
	set_thread_constraints(); /* cpus and nodes for current thread */
}

int
numa_num_configured_nodes(void)
{
	return maxconfigurednode+1;
}

int
numa_num_configured_cpus(void)
{

	return maxconfiguredcpu+1;
}

int
numa_num_possible_nodes(void)
{
	return nodemask_sz;
}

int
numa_num_possible_cpus(void)
{
	return cpumask_sz;
}

int
numa_num_thread_nodes(void)
{
	return maxprocnode+1;
}

int
numa_num_thread_cpus(void)
{
	return maxproccpu+1;
}

/*
 * Return the number of the highest node in this running system,
 */
int
numa_max_node(void)
{
	return numa_num_configured_nodes()-1;
}

make_internal_alias(numa_max_node);

/*
 * Return the number of the highest possible node in a system,
 * which for v1 is the size of a numa.h nodemask_t(in bits)-1.
 * but for v2 is the size of a kernel nodemask_t(in bits)-1.
 */
int
numa_max_possible_node_v1(void)
{
	return ((sizeof(nodemask_t)*8)-1);
}
__asm__(".symver numa_max_possible_node_v1,numa_max_possible_node@libnuma_1.1");

int
numa_max_possible_node_v2(void)
{
	return numa_num_possible_nodes()-1;
}
__asm__(".symver numa_max_possible_node_v2,numa_max_possible_node@@libnuma_1.2");

make_internal_alias(numa_max_possible_node_v1);
make_internal_alias(numa_max_possible_node_v2);

/*
 * Allocate a bitmask for cpus, of a size large enough to
 * match the kernel's cpumask_t.
 */
struct bitmask *
numa_allocate_cpumask()
{
	int ncpus = numa_num_possible_cpus();

	return numa_bitmask_alloc(ncpus);
}

/*
 * Allocate a bitmask the size of a libnuma nodemask_t
 */
static struct bitmask *
allocate_nodemask_v1(void)
{
	int nnodes = numa_max_possible_node_v1_int()+1;

	return numa_bitmask_alloc(nnodes);
}

/*
 * Allocate a bitmask for nodes, of a size large enough to
 * match the kernel's nodemask_t.
 */
struct bitmask *
numa_allocate_nodemask(void)
{
	struct bitmask *bmp;
	int nnodes = numa_max_possible_node_v2_int() + 1;

	bmp = numa_bitmask_alloc(nnodes);
	return bmp;
}

/* (cache the result?) */
long long numa_node_size64(int node, long long *freep)
{ 
	size_t len = 0;
	char *line = NULL;
	long long size = -1;
	FILE *f; 
	char fn[64];
	int ok = 0;
	int required = freep ? 2 : 1; 

	if (freep) 
		*freep = -1; 
	sprintf(fn,"/sys/devices/system/node/node%d/meminfo", node); 
	f = fopen(fn, "r");
	if (!f)
		return -1; 
	while (getdelim(&line, &len, '\n', f) > 0) { 
		char *end;
		char *s = strcasestr(line, "kB"); 
		if (!s) 
			continue; 
		--s; 
		while (s > line && isspace(*s))
			--s;
		while (s > line && isdigit(*s))
			--s; 
		if (strstr(line, "MemTotal")) { 
			size = strtoull(s,&end,0) << 10; 
			if (end == s) 
				size = -1;
			else
				ok++; 
		}
		if (freep && strstr(line, "MemFree")) { 
			*freep = strtoull(s,&end,0) << 10; 
			if (end == s) 
				*freep = -1;
			else
				ok++; 
		}
	} 
	fclose(f); 
	free(line);
	if (ok != required)
		numa_warn(W_badmeminfo, "Cannot parse sysfs meminfo (%d)", ok);
	return size;
}

make_internal_alias(numa_node_size64);

long numa_node_size(int node, long *freep)
{	
	long long f2;	
	long sz = numa_node_size64_int(node, &f2);
	if (freep) 
		*freep = f2; 
	return sz;	
}

int numa_available(void)
{
	if (get_mempolicy(NULL, NULL, 0, 0, 0) < 0 && errno == ENOSYS)
		return -1; 
	return 0;
} 

void
numa_interleave_memory_v1(void *mem, size_t size, const nodemask_t *mask)
{
	struct bitmask bitmask;

	bitmask.size = sizeof(nodemask_t) * 8;
	bitmask.maskp = (unsigned long *)mask;
	dombind(mem, size, MPOL_INTERLEAVE, &bitmask);
}
__asm__(".symver numa_interleave_memory_v1,numa_interleave_memory@libnuma_1.1");

void
numa_interleave_memory_v2(void *mem, size_t size, struct bitmask *bmp)
{ 
	dombind(mem, size, MPOL_INTERLEAVE, bmp);
} 
__asm__(".symver numa_interleave_memory_v2,numa_interleave_memory@@libnuma_1.2");

void numa_tonode_memory(void *mem, size_t size, int node)
{
	struct bitmask *nodes;

	nodes = numa_allocate_nodemask();
	numa_bitmask_setbit(nodes, node);
	dombind(mem, size, bind_policy, nodes);
}

void
numa_tonodemask_memory_v1(void *mem, size_t size, const nodemask_t *mask)
{
	struct bitmask bitmask;

	bitmask.maskp = (unsigned long *)mask;
	bitmask.size  = sizeof(nodemask_t);
	dombind(mem, size,  bind_policy, &bitmask);
}
__asm__(".symver numa_tonodemask_memory_v1,numa_tonodemask_memory@libnuma_1.1");

void
numa_tonodemask_memory_v2(void *mem, size_t size, struct bitmask *bmp)
{
	dombind(mem, size,  bind_policy, bmp);
}
__asm__(".symver numa_tonodemask_memory_v2,numa_tonodemask_memory@@libnuma_1.2");

void numa_setlocal_memory(void *mem, size_t size)
{
	dombind(mem, size, MPOL_PREFERRED, NULL);
}

void numa_police_memory(void *mem, size_t size)
{
	int pagesize = numa_pagesize_int();
	unsigned long i; 
	for (i = 0; i < size; i += pagesize)
		asm volatile("" :: "r" (((volatile unsigned char *)mem)[i]));
}

make_internal_alias(numa_police_memory);

void *numa_alloc(size_t size)
{
	char *mem;
	mem = mmap(0, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS,
		   0, 0); 
	if (mem == (char *)-1)
		return NULL;
	numa_police_memory_int(mem, size);
	return mem;
} 

void *numa_alloc_interleaved_subset_v1(size_t size, const nodemask_t *mask)
{
	char *mem;
	struct bitmask bitmask;

	mem = mmap(0, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS,
			0, 0);
	if (mem == (char *)-1)
		return NULL;
	bitmask.maskp = (unsigned long *)mask;
	bitmask.size  = sizeof(nodemask_t);
	dombind(mem, size, MPOL_INTERLEAVE, &bitmask);
	return mem;
}
__asm__(".symver numa_alloc_interleaved_subset_v1,numa_alloc_interleaved_subset@libnuma_1.1");

void *numa_alloc_interleaved_subset_v2(size_t size, struct bitmask *bmp)
{ 
	char *mem;	

	mem = mmap(0, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS,
		   0, 0); 
	if (mem == (char *)-1) 
		return NULL;
	dombind(mem, size, MPOL_INTERLEAVE, bmp);
	return mem;
} 
__asm__(".symver numa_alloc_interleaved_subset_v2,numa_alloc_interleaved_subset@@libnuma_1.2");

make_internal_alias(numa_alloc_interleaved_subset_v1);
make_internal_alias(numa_alloc_interleaved_subset_v2);

void *
numa_alloc_interleaved(size_t size)
{ 
	return numa_alloc_interleaved_subset_v2_int(size, numa_all_nodes_ptr);
} 

/*
 * given a user node mask, set memory policy to use those nodes
 */
void
numa_set_interleave_mask_v1(nodemask_t *mask)
{
	struct bitmask *bmp;
	int nnodes = numa_max_possible_node_v1_int()+1;

	bmp = numa_bitmask_alloc(nnodes);
	copy_nodemask_to_bitmask(mask, bmp);
	if (numa_bitmask_equal(bmp, numa_no_nodes_ptr))
		setpol(MPOL_DEFAULT, bmp);
	else
		setpol(MPOL_INTERLEAVE, bmp);
	numa_bitmask_free(bmp);
}

__asm__(".symver numa_set_interleave_mask_v1,numa_set_interleave_mask@libnuma_1.1");

void
numa_set_interleave_mask_v2(struct bitmask *bmp)
{
	if (numa_bitmask_equal(bmp, numa_no_nodes_ptr))
		setpol(MPOL_DEFAULT, bmp);
	else
		setpol(MPOL_INTERLEAVE, bmp);
} 
__asm__(".symver numa_set_interleave_mask_v2,numa_set_interleave_mask@@libnuma_1.2");

nodemask_t
numa_get_interleave_mask_v1(void)
{
	int oldpolicy;
	struct bitmask *bmp;
	nodemask_t mask;

	bmp = allocate_nodemask_v1();
	getpol(&oldpolicy, bmp);
	if (oldpolicy == MPOL_INTERLEAVE)
		copy_bitmask_to_nodemask(bmp, &mask);
	else
	 	copy_bitmask_to_nodemask(numa_no_nodes_ptr, &mask);
	return mask;
}
__asm__(".symver numa_get_interleave_mask_v1,numa_get_interleave_mask@libnuma_1.1");

struct bitmask *
numa_get_interleave_mask_v2(void)
{ 
	int oldpolicy;
	struct bitmask *bmp;

	bmp = numa_allocate_nodemask();
	getpol(&oldpolicy, bmp);
	if (oldpolicy == MPOL_INTERLEAVE)
		return bmp;
	return numa_no_nodes_ptr;
} 
__asm__(".symver numa_get_interleave_mask_v2,numa_get_interleave_mask@@libnuma_1.2");

/* (undocumented) */
int numa_get_interleave_node(void)
{ 
	int nd;
	if (get_mempolicy(&nd, NULL, 0, 0, MPOL_F_NODE) == 0)
		return nd;
	return 0;	
} 

void *numa_alloc_onnode(size_t size, int node) 
{ 
	char *mem; 
	struct bitmask *bmp;

	bmp = numa_allocate_nodemask();
	numa_bitmask_setbit(bmp, node);
	mem = mmap(0, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS,
		   0, 0);  
	if (mem == (char *)-1)
		return NULL;		
	dombind(mem, size, bind_policy, bmp);
	return mem; 	
} 

void *numa_alloc_local(size_t size) 
{ 
	char *mem; 
	mem = mmap(0, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS,
		   0, 0); 
	if (mem == (char *)-1)
		return NULL;
	dombind(mem, size, MPOL_PREFERRED, NULL);
	return mem; 	
} 

void numa_set_bind_policy(int strict) 
{ 
	if (strict) 
		bind_policy = MPOL_BIND; 
	else
		bind_policy = MPOL_PREFERRED;
} 

void
numa_set_membind_v1(const nodemask_t *mask)
{
	struct bitmask bitmask;

	bitmask.maskp = (unsigned long *)mask;
	bitmask.size  = sizeof(nodemask_t);
	setpol(MPOL_BIND, &bitmask);
}
__asm__(".symver numa_set_membind_v1,numa_set_membind@libnuma_1.1");

void
numa_set_membind_v2(struct bitmask *bmp)
{ 
	setpol(MPOL_BIND, bmp);
} 
__asm__(".symver numa_set_membind_v2,numa_set_membind@@libnuma_1.2");

make_internal_alias(numa_set_membind_v2);

/*
 * copy a bitmask map body to a numa.h nodemask_t structure
 */
void
copy_bitmask_to_nodemask(struct bitmask *bmp, nodemask_t *nmp)
{
	int max, i;

	memset(nmp, 0, sizeof(nodemask_t));
        max = (sizeof(nodemask_t)*8);
	for (i=0; i<bmp->size; i++) {
		if (i >= max)
			break;
		if (numa_bitmask_isbitset(bmp, i))
                	nodemask_set_v1((nodemask_t *)nmp, i);
	}
}

/*
 * copy a bitmask map body to another bitmask body
 * fill a larger destination with zeroes
 */
void
copy_bitmask_to_bitmask(struct bitmask *bmpfrom, struct bitmask *bmpto)
{
	int bytes;

	if (bmpfrom->size >= bmpto->size) {
		memcpy(bmpto->maskp, bmpfrom->maskp, CPU_BYTES(bmpto->size));
	} else if (bmpfrom->size < bmpto->size) {
		bytes = CPU_BYTES(bmpfrom->size);
		memcpy(bmpto->maskp, bmpfrom->maskp, bytes);
		memset(((char *)bmpto->maskp)+bytes, 0,
					CPU_BYTES(bmpto->size)-bytes);
	} else {
		bytes = CPU_BYTES(bmpfrom->size);
		memcpy(bmpto->maskp, bmpfrom->maskp, bytes);
	}
}

/*
 * copy a numa.h nodemask_t structure to a bitmask map body
 */
void
copy_nodemask_to_bitmask(nodemask_t *nmp, struct bitmask *bmp)
{
	int max, i;

	numa_bitmask_clearall(bmp);
        max = (sizeof(nodemask_t)*8);
	if (max > bmp->size)
		max = bmp->size;
	for (i=0; i<max; i++) {
		if (nodemask_isset_v1(nmp, i))
			numa_bitmask_setbit(bmp, i);
	}
}

nodemask_t
numa_get_membind_v1(void)
{
	int oldpolicy;
	struct bitmask *bmp;
	nodemask_t *nmp;

	bmp = allocate_nodemask_v1();
	getpol(&oldpolicy, bmp);
	if (oldpolicy == MPOL_BIND) {
		nmp = (nodemask_t *)bmp->maskp;
		return *nmp;
	}
	/* copy the body of the map to numa_all_nodes */
	copy_bitmask_to_nodemask(bmp, &numa_all_nodes);
	return numa_all_nodes;
}
__asm__(".symver numa_get_membind_v1,numa_get_membind@libnuma_1.1");

struct bitmask *
numa_get_membind_v2(void)
{
	int oldpolicy;
	struct bitmask *bmp;

	bmp = numa_allocate_nodemask();
	getpol(&oldpolicy, bmp);
	if (oldpolicy == MPOL_BIND)
		return bmp;
	return numa_all_nodes_ptr;
} 
__asm__(".symver numa_get_membind_v2,numa_get_membind@@libnuma_1.2");

//TODO:  Cliff:  do I need a v1 nodemask_t version?
struct bitmask *numa_get_mems_allowed(void)
{
	struct bitmask *bmp;

	/*
	 * can change, so query on each call.
	 */
	bmp = numa_allocate_nodemask();
	getpol(NULL,  bmp);
	return bmp;
}
make_internal_alias(numa_get_mems_allowed);


void numa_free(void *mem, size_t size)
{ 
	munmap(mem, size); 
} 

int
numa_parse_bitmap_v1(char *line, unsigned long *mask, int ncpus)
{
	int i;
	char *p = strchr(line, '\n');
	if (!p)
		return -1;

	for (i = 0; p > line;i++) {
		char *oldp, *endp;
		oldp = p;
		if (*p == ',')
			--p;
		while (p > line && *p != ',')
			--p;
		/* Eat two 32bit fields at a time to get longs */
		if (p > line && sizeof(unsigned long) == 8) {
			oldp--;
			memmove(p, p+1, oldp-p+1);
			while (p > line && *p != ',')
				--p;
		}
		if (*p == ',')
			p++;
		if (i >= CPU_LONGS(ncpus))
			return -1;
		mask[i] = strtoul(p, &endp, 16);
		if (endp != oldp)
			return -1;
		p--;
	}
	return 0;
}
__asm__(".symver numa_parse_bitmap_v1,numa_parse_bitmap@libnuma_1.1");

int
numa_parse_bitmap_v2(char *line, struct bitmask *mask)
{
	int i, ncpus;
	char *p = strchr(line, '\n'); 
	if (!p)
		return -1;
	ncpus = mask->size;

	for (i = 0; p > line;i++) {
		char *oldp, *endp; 
		oldp = p;
		if (*p == ',') 
			--p;
		while (p > line && *p != ',')
			--p;
		/* Eat two 32bit fields at a time to get longs */
		if (p > line && sizeof(unsigned long) == 8) {
			oldp--;
			memmove(p, p+1, oldp-p+1);
			while (p > line && *p != ',')
				--p;
		}
		if (*p == ',')
			p++;
		if (i >= CPU_LONGS(ncpus))
			return -1;
		mask->maskp[i] = strtoul(p, &endp, 16);
		if (endp != oldp)
			return -1;
		p--;
	}
	return 0;
}
__asm__(".symver numa_parse_bitmap_v2,numa_parse_bitmap@@libnuma_1.2");

void
init_node_cpu_mask_v2(void)
{
	int nnodes = numa_max_possible_node_v2_int() + 1;
	node_cpu_mask_v2 = calloc (nnodes, sizeof(struct bitmask *));
}

/* This would be better with some locking, but I don't want to make libnuma
   dependent on pthreads right now. The races are relatively harmless. */
int
numa_node_to_cpus_v1(int node, unsigned long *buffer, int bufferlen)
{
	int err = 0;
	char fn[64];
	FILE *f;
	char *line = NULL;
	size_t len = 0;
	struct bitmask bitmask;
	int buflen_needed;
	unsigned long *mask;
	int ncpus = numa_num_possible_cpus();
	int maxnode = numa_max_node_int();

	buflen_needed = CPU_BYTES(ncpus);
	if ((unsigned)node > maxnode || bufferlen < buflen_needed) {
		errno = ERANGE;
		return -1;
	}
	if (bufferlen > buflen_needed)
		memset(buffer, 0, bufferlen);
	if (node_cpu_mask_v1[node]) {
		memcpy(buffer, node_cpu_mask_v1[node], buflen_needed);
		return 0;
	}

	mask = malloc(buflen_needed);
	if (!mask)
		mask = (unsigned long *)buffer;
	memset(mask, 0, buflen_needed);

	sprintf(fn, "/sys/devices/system/node/node%d/cpumap", node);
	f = fopen(fn, "r");
	if (!f || getdelim(&line, &len, '\n', f) < 1) {
		numa_warn(W_nosysfs2,
		   "/sys not mounted or invalid. Assuming one node: %s",
			  strerror(errno));
		bitmask.maskp = (unsigned long *)mask;
		bitmask.size  = buflen_needed * 8;
		numa_bitmask_setall(&bitmask);
		err = -1;
	}
	if (f)
		fclose(f);

	if (line && (numa_parse_bitmap_v1(line, mask, ncpus) < 0)) {
		numa_warn(W_cpumap, "Cannot parse cpumap. Assuming one node");
		bitmask.maskp = (unsigned long *)mask;
		bitmask.size  = buflen_needed * 8;
		numa_bitmask_setall(&bitmask);
		err = -1;
	}

	free(line);
	memcpy(buffer, mask, buflen_needed);

	/* slightly racy, see above */
	if (node_cpu_mask_v1[node]) {
		if (mask != buffer)
			free(mask);
	} else {
		node_cpu_mask_v1[node] = mask;
	}
	return err;
}
__asm__(".symver numa_node_to_cpus_v1,numa_node_to_cpus@libnuma_1.1");

/*
 * test whether a node has cpus
 */
/* This would be better with some locking, but I don't want to make libnuma
   dependent on pthreads right now. The races are relatively harmless. */
/*
 * deliver a bitmask of cpus representing the cpus on a given node
 */
int
numa_node_to_cpus_v2(int node, struct bitmask *buffer)
{
	int err = 0, bufferlen;
	int nnodes = numa_num_configured_nodes();
	char fn[64], *line = NULL;
	FILE *f; 
	size_t len = 0; 
	struct bitmask *mask;

	if (!node_cpu_mask_v2)
		init_node_cpu_mask_v2();

	bufferlen = numa_bitmask_nbytes(buffer);
	if (node > nnodes-1) {
		errno = ERANGE;
		return -1;
	}
	numa_bitmask_clearall(buffer);

	if (node_cpu_mask_v2[node]) {
		/* have already constructed a mask for this node */
		if (buffer->size != node_cpu_mask_v2[node]->size) {
			numa_error("map size mismatch; abort\n");
			return -1;
		}
		memcpy(buffer->maskp, node_cpu_mask_v2[node]->maskp, bufferlen);
		return 0;
	}

	/* need a new mask for this node */
	mask = numa_allocate_cpumask();

	/* this is a kernel cpumask_t (see node_read_cpumap()) */
	sprintf(fn, "/sys/devices/system/node/node%d/cpumap", node); 
	f = fopen(fn, "r"); 
	if (!f || getdelim(&line, &len, '\n', f) < 1) { 
		numa_warn(W_nosysfs2,
		   "/sys not mounted or invalid. Assuming one node: %s",
			  strerror(errno)); 
		numa_bitmask_setall(mask);
		err = -1;
	} 
	if (f)
		fclose(f);

	if (line && (numa_parse_bitmap_v2(line, mask) < 0)) {
		numa_warn(W_cpumap, "Cannot parse cpumap. Assuming one node");
		numa_bitmask_setall(mask);
		err = -1;
	}

	free(line);
	copy_bitmask_to_bitmask(mask, buffer);

	/* slightly racy, see above */ 
	/* save the mask we created */
	if (node_cpu_mask_v2[node]) {
		/* how could this be? */
		if (mask != buffer)
			numa_bitmask_free(mask);
	} else {
		node_cpu_mask_v2[node] = mask;
	} 
	return err; 
}
__asm__(".symver numa_node_to_cpus_v2,numa_node_to_cpus@@libnuma_1.2");

make_internal_alias(numa_node_to_cpus_v1);
make_internal_alias(numa_node_to_cpus_v2);


int
numa_run_on_node_mask_v1(const nodemask_t *mask)
{
	int ncpus = numa_num_possible_cpus();
	int i, k, err;
	unsigned long cpus[CPU_LONGS(ncpus)], nodecpus[CPU_LONGS(ncpus)];
	memset(cpus, 0, CPU_BYTES(ncpus));
	for (i = 0; i < NUMA_NUM_NODES; i++) {
		if (mask->n[i / BITS_PER_LONG] == 0)
			continue;
		if (nodemask_isset_v1(mask, i)) {
			if (numa_node_to_cpus_v1_int(i, nodecpus, CPU_BYTES(ncpus)) < 0) {
				numa_warn(W_noderunmask,
					  "Cannot read node cpumask from sysfs");
				continue;
			}
			for (k = 0; k < CPU_LONGS(ncpus); k++)
				cpus[k] |= nodecpus[k];
		}
	}
	err = numa_sched_setaffinity_v1(0, CPU_BYTES(ncpus), cpus);

	/* The sched_setaffinity API is broken because it expects
	   the user to guess the kernel cpuset size. Do this in a
	   brute force way. */
	if (err < 0 && errno == EINVAL) {
		int savederrno = errno;
		char *bigbuf;
		static int size = -1;
		if (size == -1)
			size = CPU_BYTES(ncpus) * 2;
		bigbuf = malloc(CPU_BUFFER_SIZE);
		if (!bigbuf) {
			errno = ENOMEM;
			return -1;
		}
		errno = savederrno;
		while (size <= CPU_BUFFER_SIZE) {
			memcpy(bigbuf, cpus, CPU_BYTES(ncpus));
			memset(bigbuf + CPU_BYTES(ncpus), 0,
			       CPU_BUFFER_SIZE - CPU_BYTES(ncpus));
			err = numa_sched_setaffinity_v1_int(0, size, (unsigned long *)bigbuf);
			if (err == 0 || errno != EINVAL)
				break;
			size *= 2;
		}
		savederrno = errno;
		free(bigbuf);
		errno = savederrno;
	}
	return err;
}
__asm__(".symver numa_run_on_node_mask_v1,numa_run_on_node_mask@libnuma_1.1");

/*
 * Given a node mask (size of a kernel nodemask_t) (probably populated by
 * a user argument list) set up a map of cpus (map "cpus") on those nodes.
 * Then set affinity to those cpus.
 */
int
numa_run_on_node_mask_v2(struct bitmask *bmp)
{ 	
	int ncpus, i, k, err;
	struct bitmask *cpus, *nodecpus;

	cpus = numa_allocate_cpumask();
	ncpus = cpus->size;
	nodecpus = numa_allocate_cpumask();

	for (i = 0; i < bmp->size; i++) {
		if (bmp->maskp[i / BITS_PER_LONG] == 0)
			continue;
		if (numa_bitmask_isbitset(bmp, i)) {
			if (numa_node_to_cpus_v2_int(i, nodecpus) < 0) {
				numa_warn(W_noderunmask, 
					"Cannot read node cpumask from sysfs");
				continue;
			}
			for (k = 0; k < CPU_LONGS(ncpus); k++)
				cpus->maskp[k] |= nodecpus->maskp[k];
		}	
	}
	err = numa_sched_setaffinity_v2_int(0, cpus);

	/* used to have to consider that this could fail - it shouldn't now */
	if (err < 0) {
		numa_error("numa_sched_setaffinity_v2_int() failed; abort\n");
		return -1;
	}
	numa_bitmask_free(cpus);
	numa_bitmask_free(nodecpus);

	return err;
} 
__asm__(".symver numa_run_on_node_mask_v2,numa_run_on_node_mask@@libnuma_1.2");

make_internal_alias(numa_run_on_node_mask_v2);

nodemask_t
numa_get_run_node_mask_v1(void)
{
	int ncpus = numa_num_configured_cpus();
	int i, k;
	int max = numa_max_node_int();
	struct bitmask *bmp, *cpus, *nodecpus;
	nodemask_t *nmp;

	bmp = allocate_nodemask_v1(); /* the size of a nodemask_t */
	cpus = numa_allocate_cpumask();
	nodecpus = numa_allocate_cpumask();
	if (numa_sched_getaffinity_v2_int(0, cpus) < 0)
		return numa_no_nodes;

	for (i = 0; i <= max; i++) {
		if (numa_node_to_cpus_v2_int(i, nodecpus) < 0) {
			/* It's possible for the node to not exist */
			continue;
		}
		for (k = 0; k < CPU_LONGS(ncpus); k++) {
			if (nodecpus->maskp[k] & cpus->maskp[k])
				numa_bitmask_setbit(bmp, i);
		}
	}
	nmp = (nodemask_t *)bmp->maskp;
	return *nmp;
}
__asm__(".symver numa_get_run_node_mask_v1,numa_get_run_node_mask@libnuma_1.1");

struct bitmask *
numa_get_run_node_mask_v2(void)
{ 
	int i, k;
	int ncpus = numa_num_configured_cpus();
	int max = numa_max_node_int();
	struct bitmask *bmp, *cpus, *nodecpus;

	bmp = numa_allocate_cpumask();
	cpus = numa_allocate_cpumask();
	nodecpus = numa_allocate_cpumask();

	if (numa_sched_getaffinity_v2_int(0, cpus) < 0)
		return numa_no_nodes_ptr;

	for (i = 0; i <= max; i++) {
		if (numa_node_to_cpus_v2_int(i, nodecpus) < 0) {
			/* It's possible for the node to not exist */
			continue;
		}
		for (k = 0; k < CPU_LONGS(ncpus); k++) {
			if (nodecpus->maskp[k] & cpus->maskp[k])
				numa_bitmask_setbit(bmp, i);
		}
	}		
	return bmp;
} 
__asm__(".symver numa_get_run_node_mask_v2,numa_get_run_node_mask@@libnuma_1.2");

int
numa_migrate_pages(int pid, struct bitmask *fromnodes, struct bitmask *tonodes)
{
	int numa_num_nodes = numa_num_possible_nodes();

	return migrate_pages(pid, numa_num_nodes + 1, fromnodes->maskp,
							tonodes->maskp);
}

int numa_move_pages(int pid, unsigned long count,
	void **pages, const int *nodes, int *status, int flags)
{
	return move_pages(pid, count, pages, nodes, status, flags);
}

int numa_run_on_node(int node)
{ 
	int numa_num_nodes = numa_num_possible_nodes();
	struct bitmask *cpus;

	cpus = numa_allocate_cpumask();
	if (node == -1) {
		numa_bitmask_setall(cpus);
	} else if (node < numa_num_nodes) {
		if (numa_node_to_cpus_v2_int(node, cpus) < 0) {
			numa_warn(W_noderunmask,
				"Cannot read node cpumask from sysfs");
			return -1; 
		} 		
	} else { 
		errno = EINVAL;
		return -1; 
	}
	return numa_sched_setaffinity_v2_int(0, cpus);
} 

int numa_preferred(void)
{ 
	int policy;
	struct bitmask *bmp;

	bmp = numa_allocate_nodemask();
	getpol(&policy, bmp);
	if (policy == MPOL_PREFERRED || policy == MPOL_BIND) { 
		int i;
		int max = numa_num_possible_nodes();
		for (i = 0; i < max ; i++) 
			if (numa_bitmask_isbitset(bmp, i))
				return i; 
	}
	/* could read the current CPU from /proc/self/status. Probably 
	   not worth it. */
	return 0; /* or random one? */
}

void numa_set_preferred(int node)
{ 
	struct bitmask *bmp;

	bmp = numa_allocate_nodemask();
	if (node >= 0) {
		numa_bitmask_setbit(bmp, node);
		setpol(MPOL_PREFERRED, bmp);
	} else
		setpol(MPOL_DEFAULT, bmp);
	numa_bitmask_free(bmp);
} 

void numa_set_localalloc(void) 
{	
	setpol(MPOL_DEFAULT, numa_no_nodes_ptr);
} 

void numa_bind_v1(const nodemask_t *nodemask)
{
	struct bitmask bitmask;

	bitmask.maskp = (unsigned long *)nodemask;
	bitmask.size  = sizeof(nodemask_t);
	numa_run_on_node_mask_v2_int(&bitmask);
	numa_set_membind_v2_int(&bitmask);
}
__asm__(".symver numa_bind_v1,numa_bind@libnuma_1.1");

void numa_bind_v2(struct bitmask *bmp)
{
	numa_run_on_node_mask_v2_int(bmp);
	numa_set_membind_v2_int(bmp);
}
__asm__(".symver numa_bind_v2,numa_bind@@libnuma_1.2");

void numa_set_strict(int flag)
{
	if (flag)
		mbind_flags |= MPOL_MF_STRICT;
	else
		mbind_flags &= ~MPOL_MF_STRICT;
}

/*
 * Extract a node or processor number from the given string.
 * Allow a relative node / processor specification within the allowed
 * set if "relative" is nonzero
 */
static unsigned long get_nr(char *s, char **end, struct bitmask *bmp, int relative)
{
	long i, nr;

	if (!relative)
		return strtoul(s, end, 0);

	nr = strtoul(s, end, 0);
	if (s == *end)
		return nr;
	/* Find the nth set bit */
	for (i = 0; nr >= 0 && i <= bmp->size; i++)
		if (numa_bitmask_isbitset(bmp, i))
			nr--;
	return i-1;
}

/*
 * numa_parse_nodestring() is called to create a node mask, given
 * an ascii string such as 25 or 12-15 or 1,3,5-7 or +6-10.
 * (the + indicates that the numbers are cpuset-relative)
 *
 * The nodes may be specified as absolute, or relative to the current cpuset.
 * The list of available nodes is in a map pointed to by "numa_all_nodes_ptr",
 * which may represent all nodes or the nodes in the current cpuset.
 *
 * The caller must free the returned bitmask.
 */
struct bitmask *
numa_parse_nodestring(char *s)
{
	int maxnode = numa_max_node();
	int invert = 0, relative = 0;
	int conf_nodes = numa_num_configured_nodes();
	int thread_nodes = numa_num_thread_nodes();
	char *end;
	struct bitmask *mask;

	mask = numa_allocate_nodemask();

	if (s[0] == 0)
		return numa_no_nodes_ptr;
	if (*s == '!') {
		invert = 1;
		s++;
	}
	if (*s == '+') {
		relative++;
		s++;
	}
	do {
		unsigned long arg;
		int i;
		if (!strcmp(s,"all")) {
			int i;
			for (i = 0; i < thread_nodes; i++)
				numa_bitmask_setbit(mask, i);
			s+=4;
			break;
		}
		arg = get_nr(s, &end, numa_all_nodes_ptr, relative);
		if (end == s) {
			numa_warn(W_nodeparse, "unparseable node description `%s'\n", s);
			goto err;
		}
		if (arg > maxnode) {
			numa_warn(W_nodeparse, "node argument %d is out of range\n", arg);
			goto err;
		}
		i = arg;
		numa_bitmask_setbit(mask, i);
		s = end;
		if (*s == '-') {
			char *end2;
			unsigned long arg2;
			arg2 = get_nr(++s, &end2, numa_all_nodes_ptr, relative);
			if (end2 == s) {
				numa_warn(W_nodeparse, "missing node argument %s\n", s);
				goto err;
			}
			if (arg2 >= thread_nodes) {
				numa_warn(W_nodeparse, "node argument %d out of range\n", arg2);
				goto err;
			}
			while (arg <= arg2) {
				i = arg;
				if (numa_bitmask_isbitset(numa_all_nodes_ptr,i))
					numa_bitmask_setbit(mask, i);
				arg++;
			}
			s = end2;
		}
	} while (*s++ == ',');
	if (s[-1] != '\0')
		return 0;
	if (invert) {
		int i;
		for (i = 0; i < conf_nodes; i++) {
			if (numa_bitmask_isbitset(mask, i))
				numa_bitmask_clearbit(mask, i);
			else
				numa_bitmask_setbit(mask, i);
		}
	}
	return mask;

err:
	numa_bitmask_free(mask);
	return NULL;
}

/*
 * numa_parse_cpustring() is called to create a bitmask, given
 * an ascii string such as 25 or 12-15 or 1,3,5-7 or +6-10.
 * (the + indicates that the numbers are cpuset-relative)
 *
 * The cpus may be specified as absolute, or relative to the current cpuset.
 * The list of available cpus for this task is in the map pointed to by
 * "numa_all_cpus_ptr", which may represent all cpus or the cpus in the
 * current cpuset.
 *
 * The caller must free the returned bitmask.
 */
struct bitmask *
numa_parse_cpustring(char *s)
{
	int invert = 0, relative=0;
	int conf_cpus = numa_num_configured_cpus();
	int thread_cpus = numa_num_thread_cpus();
	char *end;
	struct bitmask *mask;

	mask = numa_allocate_cpumask();

	if (s[0] == 0)
		return mask;
	if (*s == '!') {
		invert = 1;
		s++;
	}
	if (*s == '+') {
		relative++;
		s++;
	}
	do {
		unsigned long arg;
		int i;

		if (!strcmp(s,"all")) {
			int i;
			for (i = 0; i < thread_cpus; i++)
				numa_bitmask_setbit(mask, i);
			s+=4;
			break;
		}
		arg = get_nr(s, &end, numa_all_cpus_ptr, relative);
		if (end == s) {
			numa_warn(W_cpuparse, "unparseable cpu description `%s'\n", s);
			goto err;
		}
		if (arg >= thread_cpus) {
			numa_warn(W_cpuparse, "cpu argument %s is out of range\n", s);
			goto err;
		}
		i = arg;
		numa_bitmask_setbit(mask, i);
		s = end;
		if (*s == '-') {
			char *end2;
			unsigned long arg2;
			int i;
			arg2 = get_nr(++s, &end2, numa_all_cpus_ptr, relative);
			if (end2 == s) {
				numa_warn(W_cpuparse, "missing cpu argument %s\n", s);
				goto err;
			}
			if (arg2 >= thread_cpus) {
				numa_warn(W_cpuparse, "cpu argument %s out of range\n", s);
				goto err;
			}
			while (arg <= arg2) {
				i = arg;
				if (numa_bitmask_isbitset(numa_all_cpus_ptr, i))
					numa_bitmask_setbit(mask, i);
				arg++;
			}
			s = end2;
		}
	} while (*s++ == ',');
	if (s[-1] != '\0')
		return 0;
	if (invert) {
		int i;
		for (i = 0; i < conf_cpus; i++) {
			if (numa_bitmask_isbitset(mask, i))
				numa_bitmask_clearbit(mask, i);
			else
				numa_bitmask_setbit(mask, i);
		}
	}
	return mask;

err:
	numa_bitmask_free(mask);
	return NULL;
}
