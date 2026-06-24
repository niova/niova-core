noinst_LTLIBRARIES =
noinst_PROGRAMS =
TESTS =
lib_LTLIBRARIES =
libexec_PROGRAMS =
pkgconfig_DATA = ./niova.pc
# libexec installs are placed into the 'niova' parent directory
my_libexecdir = $(libexecdir)/niova
my_libexec_PROGRAMS =

my_includedir = $(includedir)/niova
my_include_HEADERS =

ACLOCAL_AMFLAGS = -I m4

if ARCH_X86
ARCH_SOURCES = src/contrib/crc32c-pcl-intel-asm_64.S \
        src/contrib/crct10dif-pcl-asm_64.S
else
if ARCH_ARM
ARCH_SOURCES = src/contrib/crc32c_arm64.c \
	       src/contrib/crct10dif-ce-arm64.S
AM_CFLAGS += -march=armv8-a+crc+crypto
AM_CCASFLAGS = -march=armv8-a+aes+crc+crypto

else
ARCH_SOURCES =
endif
endif

CORE_HDRS = src/include/atomic.h \
	src/include/alloc.h \
        src/include/binary_hist.h \
        src/include/bitmap.h \
        src/include/buffer.h \
        src/include/common.h \
	src/include/config_token.h \
        src/include/crc24q.h \
        src/include/crc32.h \
        src/include/ec.h \
        src/include/ctl_interface_cmd.h \
	src/include/ctl_interface.h \
        src/include/ctl_svc.h \
        src/include/ctor.h \
        src/include/env.h \
        src/include/epoll_mgr.h \
        src/include/ev_pipe.h \
	src/include/fault_inject.h \
        src/include/file_util.h \
        src/include/io.h \
        src/include/init.h \
	src/include/lock.h \
        src/include/log.h \
	src/include/net_ctl.h \
	src/include/niova_backtrace.h \
        src/include/popen_cmd.h \
	src/include/queue.h \
	src/include/random.h \
        src/include/ref_tree_proto.h \
	src/include/regex_defines.h \
        src/include/registry.h \
	src/include/system_info.h \
	src/include/thread.h \
        src/include/udp.h \
        src/include/tcp.h \
        src/include/tcp_mgr.h \
	src/include/util.h \
        src/include/util_thread.h \
        src/include/watchdog.h \
	src/contrib/include/tree.h \
	src/contrib/include/dlmalloc.h

CORE_SOURCES = $(CORE_HDRS) \
	src/alloc.c \
        src/buffer.c \
        src/config_token.c \
        src/contrib/crc24q.c \
        src/ec.c \
        $(ARCH_SOURCES) \
        src/contrib/dlmalloc.c \
        src/ctl_interface.c \
        src/ctl_interface_cmd.c \
        src/ctl_svc.c \
        src/env.c \
        src/epoll_mgr.c \
	src/ev_pipe.c \
        src/fault_inject.c \
        src/file_util.c \
        src/init.c \
        src/io.c \
        src/log.c \
	src/popen_cmd.c \
	src/random.c \
        src/registry.c \
	src/system_info.c \
        src/thread.c \
	src/udp.c \
	src/tcp.c \
        src/tcp_mgr.c \
        src/util_thread.c \
	src/watchdog.c

my_include_HEADERS += $(CORE_HDRS)

lib_LTLIBRARIES += src/libniova.la
src_libniova_la_LDFLAGS = \
	-version-info @MAJOR_VERSION@:@MINOR_VERSION@:@VERSION_SUFFIX@
src_libniova_la_LIBADD = $(ISAL_LIBS)
src_libniova_la_SOURCES = $(CORE_SOURCES)

## Non-library sources
lib_LTLIBRARIES += src/libniova_bt.la
src_libniova_bt_la_LDFLAGS = \
	-version-info @MAJOR_VERSION@:@MINOR_VERSION@:@VERSION_SUFFIX@
src_libniova_bt_la_SOURCES = src/niova_backtrace.c \
	src/include/src/niova_backtrace.h

## Tests
noinst_PROGRAMS += test/ref-tree-test
test_ref_tree_test_SOURCES = test/ref-tree-test.c
test_ref_tree_test_LDADD = src/libniova.la
TESTS += test/ref-tree-test

noinst_PROGRAMS += test/ev-pipe-test
test_ev_pipe_test_SOURCES =  test/ev-pipe-test.c
test_ev_pipe_test_LDADD = src/libniova.la src/libniova_bt.la
TESTS += test/ev-pipe-test

noinst_PROGRAMS += test/work-dispatch-test
test_work_dispatch_test_SOURCES = test/work-dispatch-test.c
test_work_dispatch_test_LDADD = src/libniova.la
TESTS += test/work-dispatch-test

noinst_PROGRAMS += test/binary-hist-test
test_binary_hist_test_SOURCES = test/binary-hist-test.c
test_binary_hist_test_LDADD = src/libniova.la
TESTS += test/binary-hist-test

noinst_PROGRAMS += test/common-test
test_common_test_SOURCES = test/common-test.c
test_common_test_LDADD = src/libniova.la
TESTS += test/common-test

noinst_PROGRAMS += test/micro-test
test_micro_test_SOURCES = test/micro-test.c
test_micro_test_LDADD = src/libniova.la
TESTS += test/micro-test

noinst_PROGRAMS += test/alloc-test
test_alloc_test_SOURCES = test/alloc-test.c
test_alloc_test_LDADD = src/libniova.la
TESTS += test/alloc-test

noinst_PROGRAMS += test/buffer-test
test_buffer_test_SOURCES = test/buffer-test.c
test_buffer_test_LDADD = src/libniova.la
TESTS += test/buffer-test

noinst_PROGRAMS += test/registry-test
# Use CORE_SOURCES so that this binary's constructor runs inside of niova
# init_ctx()
test_registry_test_SOURCES = $(src_libniova_bt_la_SOURCES) $(CORE_SOURCES) \
	test/registry-test.c
# See: https://www.gnu.org/software/automake/manual/html_node/Objects-created-both-with-libtool-and-without.html
test_registry_test_CFLAGS = $(AM_CFLAGS)
test_registry_test_CPPFLAGS = $(AM_CPPFLAGS)
test_registry_test_ASFLAGS = $(AM_ASFLAGS)
TESTS += test/registry-test

noinst_PROGRAMS += test/config-token-test
test_config_token_test_SOURCES = test/config-token-test.c
test_config_token_test_LDADD = src/libniova.la src/libniova_bt.la
TESTS += test/config-token-test

noinst_PROGRAMS += test/tcp-test
test_tcp_test_SOURCES =  test/tcp-test.c
test_tcp_test_LDADD = src/libniova.la src/libniova_bt.la
TESTS += test/tcp-test

noinst_PROGRAMS += test/udp-test
test_udp_test_SOURCES =  test/udp-test.c
test_udp_test_LDADD = src/libniova.la src/libniova_bt.la
TESTS += test/udp-test

noinst_PROGRAMS += test/random-test
test_random_test_SOURCES = test/random-test.c
test_random_test_LDADD = src/libniova.la
TESTS += test/random-test

noinst_PROGRAMS += test/queue-test
test_queue_test_SOURCES = test/queue-test.c
test_queue_test_LDADD = src/libniova.la
TESTS += test/queue-test

noinst_PROGRAMS += test/util-test
test_util_test_SOURCES = test/util-test.c
test_util_test_LDADD = src/libniova.la
TESTS += test/util-test

noinst_PROGRAMS += test/regex-test
test_regex_test_SOURCES = test/regex-tests.c
test_regex_test_LDADD = src/libniova.la
TESTS += test/regex-test

noinst_PROGRAMS += test/epoll-mgr-test
test_epoll_mgr_test_SOURCES =  \
	test/epoll-mgr-test.c
test_epoll_mgr_test_LDADD = src/libniova.la src/libniova_bt.la
TESTS += test/epoll-mgr-test


noinst_PROGRAMS += test/bitmap-test
test_bitmap_test_SOURCES = test/bitmap-test.c
test_bitmap_test_LDADD = src/libniova.la
test_bitmap_test_CFLAGS = $(AM_CFLAGS)
TESTS += test/bitmap-test

noinst_PROGRAMS += test/io-iov-test
test_io_iov_test_SOURCES = test/io-iov-test.c
test_io_iov_test_LDADD = src/libniova.la
test_io_iov_test_CFLAGS = $(AM_CFLAGS)
TESTS += test/io-iov-test

noinst_PROGRAMS += test/mspace-test
test_mspace_test_SOURCES = test/mspace-test.c
test_mspace_test_LDADD = src/libniova.la
test_mspace_test_CFLAGS = $(AM_CFLAGS)
TESTS += test/mspace-test

noinst_PROGRAMS += test/rb-tree-arg-test
test_rb_tree_arg_test_SOURCES = test/rb-tree-arg-test.c
test_rb_tree_arg_test_LDADD = src/libniova.la
test_rb_tree_arg_test_CFLAGS = $(AM_CFLAGS)
TESTS += test/rb-tree-arg-test

noinst_PROGRAMS += test/ec-test
test_ec_test_SOURCES = test/ec-test.c test/ec-reference-data.h
test_ec_test_LDADD = src/libniova.la
test_ec_test_CFLAGS = $(AM_CFLAGS)
TESTS += test/ec-test

# Run ec-test as part of `make`, not just `make check`.
all-local: test/ec-test.stamp

test/ec-test.stamp: test/ec-test$(EXEEXT)
	@./test/ec-test$(EXEEXT)
	@touch $@

CLEANFILES = test/ec-test.stamp


autofmt:
	uncrustify -c tools/uncrustify.cfg --no-backup `find . -name "*.[ch]"` | tee /dev/null

autofmt-check:
	uncrustify -c tools/uncrustify.cfg -q --check `find . -name "*.[ch]"` | grep -c FAIL | grep ^0$ > /dev/null

version:
	echo "Version info: major=@MAJOR_VERSION@ minor=@MINOR_VERSION@ suffix=@VERSION_SUFFIX@"

show-flags:
	echo "AM_CFLAGS: ${AM_CFLAGS}"
	echo "CFLAGS: ${CFLAGS}"

gen-coverage:
	lcov --no-external -b . --capture --directory . \
		--output-file niova-coverage.out
	genhtml ./niova-coverage.out --output-directory ./niova-coverage
