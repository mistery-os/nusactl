
ACLOCAL_AMFLAGS = -I m4
CLEANFILES =

AM_CPPFLAGS = -Wall

bin_PROGRAMS = nusactl nusastat nusademo migratepages migspeed memhog

lib_LTLIBRARIES = libnusa.la

include_HEADERS = nusa.h nusacompat1.h nusaif.h

noinst_HEADERS = nusaint.h util.h

dist_man_MANS = move_pages.2 nusa.3 nusactl.8 nusastat.8 migratepages.8 migspeed.8 memhog.8

EXTRA_DIST = README.md INSTALL.md

nusactl_SOURCES = nusactl.c util.c shm.c shm.h
nusactl_LDADD = libnusa.la

nusastat_SOURCES = nusastat.c
nusastat_CFLAGS = $(AM_CFLAGS) -std=gnu99

nusademo_SOURCES = nusademo.c stream_lib.c stream_lib.h mt.c mt.h clearcache.c clearcache.h
nusademo_CPPFLAGS = $(AM_CPPFLAGS) -DHAVE_STREAM_LIB -DHAVE_MT -DHAVE_CLEAR_CACHE
nusademo_CFLAGS = -O3 -ffast-math -funroll-loops
if HAVE_TREE_VECTORIZE
nusademo_CFLAGS += -ftree-vectorize
endif
nusademo_LDADD = libnusa.la -lm

migratepages_SOURCES = migratepages.c util.c
migratepages_LDADD = libnusa.la

migspeed_SOURCES = migspeed.c util.c
migspeed_LDADD = libnusa.la -lrt

memhog_SOURCES = memhog.c util.c
memhog_LDADD = libnusa.la

libnusa_la_SOURCES = libnusa.c syscall.c distance.c affinity.c affinity.h sysfs.c sysfs.h rtnetlink.c rtnetlink.h versions.ldscript
libnusa_la_LDFLAGS = -version-info 1:0:0 -Wl,--version-script,$(srcdir)/versions.ldscript -Wl,-init,nusa_init -Wl,-fini,nusa_fini

check_PROGRAMS = \
	test/distance \
	test/ftok \
	test/mbind_mig_pages \
	test/migrate_pages \
	test/move_pages \
	test/mynode \
	test/node-parse \
	test/nodemap \
	test/pagesize \
	test/prefered \
	test/randmap \
	test/realloc_test \
	test/tbitmap \
	test/tshared

EXTRA_DIST += \
	test/README \
	test/bind_range \
	test/checkaffinity \
	test/checktopology \
	test/nusademo \
	test/printcpu \
	test/regress \
	test/regress2 \
	test/regress-io \
	test/runltp \
	test/shmtest

test_distance_SOURCES = test/distance.c
test_distance_LDADD = libnusa.la

test_ftok_SOURCES = test/ftok.c
test_ftok_LDADD = libnusa.la

test_mbind_mig_pages_SOURCES = test/mbind_mig_pages.c
test_mbind_mig_pages_LDADD = libnusa.la

test_migrate_pages_SOURCES = test/migrate_pages.c
test_migrate_pages_LDADD = libnusa.la

test_move_pages_SOURCES = test/move_pages.c
test_move_pages_LDADD = libnusa.la

test_mynode_SOURCES = test/mynode.c
test_mynode_LDADD = libnusa.la

test_node_parse_SOURCES = test/node-parse.c util.c
test_node_parse_LDADD = libnusa.la

test_nodemap_SOURCES = test/nodemap.c
test_nodemap_LDADD = libnusa.la

test_pagesize_SOURCES = test/pagesize.c
test_pagesize_LDADD = libnusa.la

test_prefered_SOURCES = test/prefered.c
test_prefered_LDADD = libnusa.la

test_randmap_SOURCES = test/randmap.c
test_randmap_LDADD = libnusa.la

test_realloc_test_SOURCES = test/realloc_test.c
test_realloc_test_LDADD = libnusa.la

test_tbitmap_SOURCES = test/tbitmap.c util.c
test_tbitmap_LDADD = libnusa.la

test_tshared_SOURCES = test/tshared.c
test_tshared_LDADD = libnusa.la

# Legacy make rules for test cases.
# These will be superceded by "make check".

regress1: $(check_PROGRAMS)
	cd test && ./regress

regress2: $(check_PROGRAMS)
	cd test && ./regress2

test_nusademo: nusademo
	./nusademo -t -e 10M

test: all $(check_PROGRAMS) regress1 regress2 test_nusademo

TESTS_ENVIRONMENT = builddir='$(builddir)'; export builddir;

TESTS = \
	test/bind_range \
	test/checkaffinity \
	test/checktopology \
	test/distance \
	test/move_pages \
	test/nodemap \
	test/nusademo \
	test/regress \
	test/tbitmap

# These are known to be broken:
#	test/prefered
#	test/randmap

SED_PROCESS = \
        $(AM_V_GEN)$(SED) \
        -e 's,@VERSION\@,$(VERSION),g' \
        -e 's,@prefix\@,$(prefix),g' \
        -e 's,@exec_prefix\@,$(exec_prefix),g' \
        -e 's,@libdir\@,$(libdir),g' \
        -e 's,@includedir\@,$(includedir),g' \
        < $< > $@ || rm $@

%.pc: %.pc.in Makefile
	$(SED_PROCESS)

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = nusa.pc
EXTRA_DIST += nusa.pc.in
CLEANFILES += nusa.pc
