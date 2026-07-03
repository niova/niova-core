AC_PREREQ(2.63)

m4_define([niova_core_major], [0])
m4_define([niova_core_minor], [1])
m4_define([niova_core_suffix], [0])
m4_define([niova_core_version],
          [niova_core_major.niova_core_minor.niova_core_suffix])

AC_INIT([niova_core],[niova_core_version],[],[],[pauln@niova.io])

# Dependencies
PKG_PROG_PKG_CONFIG
PKG_INSTALLDIR

AC_SUBST(MAJOR_VERSION, niova_core_major)
AC_SUBST(MINOR_VERSION, niova_core_minor)
AC_SUBST(VERSION_SUFFIX, niova_core_suffix)

AC_SUBST([AM_CFLAGS], ["-g -O2 -Wall -Wextra"])
AC_SUBST([AM_CPPFLAGS], ["-Isrc/include -Isrc/contrib/include"])
AC_SUBST([AM_LDFLAGS], [" "])

# Prevent Libtool from adding its own CFLAGS contents
: ${CXXFLAGS=""}
: ${CFLAGS=""}
# Libtool Init
LT_INIT

m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

AC_CONFIG_AUX_DIR([.])
AM_PROG_AS
AM_INIT_AUTOMAKE([foreign subdir-objects -Wall -Wno-portability])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS([niova_core_config.h])
AC_CONFIG_FILES([Makefile])

AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_CC_C99

AC_C_CONST
AC_C_INLINE
AC_TYPE_INT8_T
AC_TYPE_INT16_T
AC_TYPE_INT32_T
AC_TYPE_INT64_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_TYPE_UINT8_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T

AC_CANONICAL_HOST
AS_CASE(["$host_cpu"],
    [arm*|aarch64*], [arm=true],
    [i?86|x86_64], [x86=true]
)
AM_CONDITIONAL([ARCH_ARM], [test x$arm = xtrue])
AM_CONDITIONAL([ARCH_X86], [test x$x86 = xtrue])

AC_HEADER_STDC
AC_CHECK_HEADERS([pthread.h], [],
                 [AC_MSG_ERROR([failed to locate pthread.h])], [])
AC_CHECK_HEADERS([unistd.h], [],
                 [AC_MSG_ERROR([failed to locate unistd.h])], [])
AC_CHECK_HEADERS([string.h], [],
                 [AC_MSG_ERROR([failed to locate string.h])], [])
AC_CHECK_HEADERS([stdlib.h], [],
                 [AC_MSG_ERROR([failed to locate stdlib.h])], [])
AC_CHECK_HEADERS([uuid/uuid.h], [],
                 [AC_MSG_ERROR([failed to locate uuid/uuid.h])], [])
AC_CHECK_HEADERS([backtrace.h], [],
                 [AC_MSG_ERROR([failed to locate backtrace.h])], [])
AC_CHECK_HEADERS([zlib.h], [],
                 [AC_MSG_ERROR([failed to locate zlib.h])], [])
AC_CHECK_HEADERS([lz4.h], [],
                 [AC_MSG_ERROR([failed to locate lz4.h])], [])

# Checks for typedefs, structures, and compiler characteristics.
AC_HEADER_STAT
AC_C_CONST
AC_HEADER_STDBOOL
AC_TYPE_UID_T
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_HEADER_TIME
AC_STRUCT_TM

AC_FUNC_CHOWN
AC_FUNC_CLOSEDIR_VOID
AC_FUNC_ERROR_AT_LINE
AC_FUNC_FORK
AC_FUNC_LSTAT
AC_FUNC_LSTAT_FOLLOWS_SLASHED_SYMLINK
AC_FUNC_MALLOC
AC_FUNC_MEMCMP
AC_FUNC_MMAP
AC_TYPE_SIGNAL
AC_FUNC_STAT
AC_FUNC_STRERROR_R
AC_FUNC_STRFTIME
AC_FUNC_VPRINTF
AC_CHECK_FUNCS([endpwent ftruncate getcwd gethostbyname inet_ntoa localtime_r memmove memset mkdir munmap putenv realloc rmdir setrlimit socket strcasecmp strchr strcspn strdup strerror strncasecmp strpbrk strrchr strstr strtol tzset])

# These functions are *required* without option.
AC_CHECK_FUNCS([clock_gettime], [], AC_MSG_ERROR([unable to locate required symbol clock_gettime]))
AC_CHECK_FUNCS([reallocarray], [], AC_MSG_ERROR([unable to locate required symbol reallocarray]))

# libbacktrace
AC_CHECK_LIB([backtrace],[backtrace_full],,
             [AC_MSG_ERROR([Could not find backtrace_full])])

# libuuid
AC_CHECK_LIB([uuid],[uuid_compare],,
             [AC_MSG_ERROR([Could not find uuid_compare])])

#pthread
AC_CHECK_LIB([pthread],[pthread_join],,
             [AC_MSG_ERROR([Could not find pthread_join])])

#zlib
AC_CHECK_LIB([z],[compressBound],,
             [AC_MSG_ERROR([Could not find compressBound])])

#lz4
AC_CHECK_LIB([lz4],[LZ4_compress_default],,
             [AC_MSG_ERROR([Could not find LZ4_compress_default])])

AC_CHECK_HEADERS([isa-l.h], [],
                 [AC_MSG_ERROR([failed to locate isa-l.h])], [])

AC_CHECK_LIB([isal], [crc32_iscsi], [],
             [AC_MSG_ERROR([Could not find crc32_iscsi in -lisal])])

AC_CHECK_LIB([isal], [crc16_t10dif_base], [],
             [AC_MSG_ERROR([Could not find crc16_t10dif_base in -lisal])])

AC_CHECK_LIB([isal], [ec_init_tables],
             [AC_SUBST(ISAL_LIBS, [-lisal])],
             [AC_MSG_ERROR([Could not find ec_init_tables in -lisal])])

if [test x$arm == xtrue] ; then
AC_MSG_CHECKING([for 64-bit PMULL support])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#include <stdio.h>
#include <stdint.h>
#include <sys/auxv.h>
#include <asm/hwcap.h>
]], [[
#if defined(__aarch64__)
unsigned long hwcap = getauxval(AT_HWCAP);
if (hwcap & HWCAP_PMULL)
	return 0;
#endif
return -1;
]])
], [
AC_DEFINE([HAVE_PMULL64], [1], [Define as 1 if you have 64b PMULL support])
AC_MSG_RESULT([yes])
AM_CFLAGS="$AM_CFLAGS -DHAVE_PMULL64"
], [
AC_MSG_RESULT([no])
])

AC_MSG_CHECKING([if little-endian])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#include <stdio.h>
]], [[
#if defined(__aarch64__)
    union {
      unsigned int i;
      char c[sizeof(unsigned int)];
    } x;
    x.i = 1;
    return x.c[0];
#endif
return -1;
]])
], [
AC_DEFINE([ARM_LE], [1], [System is little endian])
AC_MSG_RESULT([yes])
], [
AC_MSG_RESULT([no])
])
fi

AC_ARG_ENABLE(
    [asan],
    [AS_HELP_STRING([--enable-asan],[address sanitizer build])],
    [
        AM_CFLAGS="$AM_CFLAGS -fsanitize=address"
        AC_MSG_NOTICE([address sanitizer enabled])
    ],
    [AC_MSG_NOTICE([address sanitizer disabled])]
)

AC_ARG_ENABLE(
    [lsan],
    [AS_HELP_STRING([--enable-lsan],[leak sanitizer build])],
    [
        AM_CFLAGS="$AM_CFLAGS -fsanitize=leak"
        AC_MSG_NOTICE([leak sanitizer enabled])
    ],
    [AC_MSG_NOTICE([leak sanitizer disabled])]
)

AC_ARG_ENABLE(
    [gcov],
    [AS_HELP_STRING([--enable-gcov],[code coverage build])],
    [
        # Overwrite AM_CFLAGS
        AM_CFLAGS="-O0 -fprofile-arcs -ftest-coverage"
        AM_LDFLAGS="$AM_LDFLAGS -lgcov"
        AC_MSG_NOTICE([code coverage enabled])
    ],
    [AC_MSG_NOTICE([code coverage disabled])]
)

AC_ARG_ENABLE(
    [devel],
    [AS_HELP_STRING([--enable-devel],[developer options])],
    [
        AM_CPPFLAGS="$AM_CPPFLAGS -DNIOVA_FAULT_INJECTION_ENABLED"
        AC_MSG_NOTICE([developer options enabled])
    ],
    [AC_MSG_NOTICE([developer options disabled])]
)

AC_ARG_ENABLE(
    [debug],
    [AS_HELP_STRING([--enable-debug],[debug options])],
    [
        AM_CPPFLAGS="$AM_CPPFLAGS -DNIOVA_FAULT_INJECTION_ENABLED"
        # Overwrite AM_CFLAGS
        AM_CFLAGS="-O0 -g3 -ggdb"
        AC_MSG_NOTICE([debug options enabled])
    ],
    [AC_MSG_NOTICE([debug options disabled])]
)

AC_CONFIG_FILES([niova.pc])

AC_OUTPUT
