AM_CPPFLAGS = \
	$(all_includes) \
	-I$(top_srcdir)/include \
	$(NULL)

AM_CFLAGS = \
	-Wall \
	-ggdb3 \
	$(LIBOSMOCORE_CFLAGS) \
	$(NULL)

LDADD = \
	$(top_builddir)/src/libmsc/libmsc.a \
	$(LIBOSMOCORE_LIBS) \
	$(NULL)

EXTRA_DIST = \
	mncc_test.ok \
	mncc_test.err \
	$(NULL)

noinst_PROGRAMS = \
	mncc_test \
	$(NULL)

mncc_test_SOURCES = \
	mncc_test.c \
	$(NULL)

.PHONY: update_exp
update_exp:
	$(builddir)/mncc_test >$(srcdir)/mncc_test.ok 2>$(srcdir)/mncc_test.err
