/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Paul Nowoczynski <00pauln00@gmail.com> 2020
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
//#include <uuid/uuid.h>

#include "crc32.h"
#include "util.h"
#include "log.h"
#include "random.h"

static void
mk_time_string_test(void)
{
    time_t now = niova_realtime_coarse_clock_get_sec();

    char time_string[MK_TIME_STR_LEN];

    int rc = niova_mk_time_string(now, time_string, MK_TIME_STR_LEN);
    if (rc)
        exit(rc);

    fprintf(stdout, "%s:  %s\n", __func__, time_string);

    rc = niova_mk_time_string(now, time_string, 1);
    if (rc != -ENOSPC)
        fprintf(stdout, "expected rc=-ENOSPC\n");
}

static void
niova_string_to_unsigned_long_long_test(void)
{
    const unsigned long long sentinel = 42;
    unsigned long long val = sentinel;

    int rc = niova_string_to_unsigned_long_long("", NULL);
    MY_FATAL_IF(rc != -EINVAL, "expected -EINVAL, got %d", rc);

    rc = niova_string_to_unsigned_long_long("foo", NULL);
    MY_FATAL_IF(rc != -EINVAL, "expected -EINVAL, got %d", rc);

    rc = niova_string_to_unsigned_long_long(NULL, &val);
    MY_FATAL_IF(rc != -EINVAL || val != sentinel,
                "expected -EINVAL and unchanged val, got %d val=%llu", rc, val);

    rc = niova_string_to_unsigned_long_long("", &val);
    MY_FATAL_IF(rc != -EINVAL || val != sentinel,
                "expected -EINVAL and unchanged val, got %d val=%llu", rc, val);

    rc = niova_string_to_unsigned_long_long("foo", &val);
    MY_FATAL_IF(rc != -EINVAL || val != sentinel,
                "expected -EINVAL and unchanged val, got %d val=%llu", rc, val);

    rc = niova_string_to_unsigned_long_long("-1", &val);
    MY_FATAL_IF(rc != -EINVAL || val != sentinel,
                "expected -EINVAL and unchanged val, got %d val=%llu", rc, val);

    rc = niova_string_to_unsigned_long_long("+1", &val);
    MY_FATAL_IF(rc != -EINVAL || val != sentinel,
                "expected -EINVAL and unchanged val, got %d val=%llu", rc, val);

    rc = niova_string_to_unsigned_long_long(" 1", &val);
    MY_FATAL_IF(rc != -EINVAL || val != sentinel,
                "expected -EINVAL and unchanged val, got %d val=%llu", rc, val);

    rc = niova_string_to_unsigned_long_long("0xdeadbeef", &val);
    MY_FATAL_IF(rc != -EINVAL || val != sentinel,
                "expected -EINVAL and unchanged val, got %d val=%llu", rc, val);

    rc = niova_string_to_unsigned_long_long("1junk", &val);
    MY_FATAL_IF(rc != -EINVAL || val != sentinel,
                "expected -EINVAL and unchanged val, got %d val=%llu", rc, val);

    rc = niova_string_to_unsigned_long_long("1.0", &val);
    MY_FATAL_IF(rc != -EINVAL || val != sentinel,
                "expected -EINVAL and unchanged val, got %d val=%llu", rc, val);

    rc = niova_string_to_unsigned_long_long("18446744073709551616", &val);
    MY_FATAL_IF(rc != -ERANGE || val != sentinel,
                "expected -ERANGE and unchanged val, got %d val=%llu", rc, val);

    rc = niova_string_to_unsigned_long_long("0", &val);
    MY_FATAL_IF(rc || val != 0, "expected 0, got %d val=%llu", rc, val);

    rc = niova_string_to_unsigned_long_long("1", &val);
    MY_FATAL_IF(rc || val != 1, "expected 1, got %d val=%llu", rc, val);

    rc = niova_string_to_unsigned_long_long("18446744073709551615", &val);
    MY_FATAL_IF(rc || val != ULLONG_MAX,
                "expected ULLONG_MAX, got %d val=%llu", rc, val);
}

static void
niova_string_to_long_long_test(void)
{
    const long long sentinel = 42;
    long long val = sentinel;

    int rc = niova_string_to_long_long(NULL, &val);
    MY_FATAL_IF(rc != -EINVAL || val != sentinel,
                "expected -EINVAL and unchanged val, got %d val=%lld", rc, val);

    const char *invalid[] = {
        "", "foo", "+", "-", " 1", "1junk", "1.0", "0x1",
    };

    for (size_t i = 0; i < ARRAY_SIZE(invalid); i++)
    {
        rc = niova_string_to_long_long(invalid[i], &val);
        MY_FATAL_IF(rc != -EINVAL || val != sentinel,
                    "input='%s' expected -EINVAL and unchanged val, "
                    "got %d val=%lld", invalid[i], rc, val);
    }

    rc = niova_string_to_long_long("9223372036854775808", &val);
    MY_FATAL_IF(rc != -ERANGE || val != sentinel,
                "expected -ERANGE and unchanged val, got %d val=%lld", rc, val);

    rc = niova_string_to_long_long("-9223372036854775809", &val);
    MY_FATAL_IF(rc != -ERANGE || val != sentinel,
                "expected -ERANGE and unchanged val, got %d val=%lld", rc, val);

    rc = niova_string_to_long_long("0", &val);
    MY_FATAL_IF(rc || val != 0, "expected 0, got %d val=%lld", rc, val);

    rc = niova_string_to_long_long("+1", &val);
    MY_FATAL_IF(rc || val != 1, "expected 1, got %d val=%lld", rc, val);

    rc = niova_string_to_long_long("-1", &val);
    MY_FATAL_IF(rc || val != -1, "expected -1, got %d val=%lld", rc, val);

    rc = niova_string_to_long_long("9223372036854775807", &val);
    MY_FATAL_IF(rc || val != LLONG_MAX,
                "expected LLONG_MAX, got %d val=%lld", rc, val);

    rc = niova_string_to_long_long("-9223372036854775808", &val);
    MY_FATAL_IF(rc || val != LLONG_MIN,
                "expected LLONG_MIN, got %d val=%lld", rc, val);
}

static void
niova_string_to_unsigned_int_test(void)
{
    const unsigned int sentinel = 42;
    unsigned int val = sentinel;

    int rc = niova_string_to_unsigned_int("", NULL);
    MY_FATAL_IF(rc != -EINVAL, "expected -EINVAL, got %d", rc);

    rc = niova_string_to_unsigned_int("1", NULL);
    MY_FATAL_IF(rc != -EINVAL, "expected -EINVAL, got %d", rc);

    rc = niova_string_to_unsigned_int(NULL, &val);
    MY_FATAL_IF(rc != -EINVAL || val != sentinel,
                "expected -EINVAL and unchanged val, got %d val=%u", rc, val);

    const char *invalid[] = {
        "", "foo", "-1", "+1", " 1", "1junk", "1.0", "0x1",
    };

    for (size_t i = 0; i < ARRAY_SIZE(invalid); i++)
    {
        rc = niova_string_to_unsigned_int(invalid[i], &val);
        MY_FATAL_IF(rc != -EINVAL || val != sentinel,
                    "input='%s' expected -EINVAL and unchanged val, "
                    "got %d val=%u", invalid[i], rc, val);
    }

    rc = niova_string_to_unsigned_int("18446744073709551615", &val);
    MY_FATAL_IF(rc != -EOVERFLOW || val != sentinel,
                "expected -EOVERFLOW and unchanged val, got %d val=%u", rc, val);

    // UINT_MAX + 1
    rc = niova_string_to_unsigned_int("4294967296", &val);
    MY_FATAL_IF(rc != -EOVERFLOW || val != sentinel,
                "expected -EOVERFLOW and unchanged val, got %d val=%u", rc, val);

    rc = niova_string_to_unsigned_int("4294967295", &val);
    MY_FATAL_IF((rc || val != UINT_MAX),
                "expected 0, got %d - val expects %u got val=%u",
                rc, UINT_MAX, val);

    rc = niova_string_to_unsigned_int("0", &val);
    MY_FATAL_IF((rc || val != 0), "expected 0, got %d val=%u", rc, val);
}

static void
niova_parse_comma_delimited_uint_string_test(void)
{
    unsigned long long val = 0;
    int rc = niova_parse_comma_delimited_uint_string(NULL, 1, &val);
    MY_FATAL_IF(rc != -EINVAL, "expected -EINVAL, got %d", rc);

    rc = niova_parse_comma_delimited_uint_string("foo", 0, &val);
    MY_FATAL_IF(rc != -EINVAL, "expected -EINVAL, got %d", rc);

    rc = niova_parse_comma_delimited_uint_string("foo", 1, NULL);
    MY_FATAL_IF(rc != -EINVAL, "expected -EINVAL, got %d", rc);

    rc = niova_parse_comma_delimited_uint_string("foo", 3, &val);
    MY_FATAL_IF(rc != -ENOENT, "expected -ENOENT, got %d", rc);

    char *str = "foo 0,11,,22,,333432432,,,";
    rc = niova_parse_comma_delimited_uint_string(str, strlen(str), &val);
    MY_FATAL_IF(rc != -ENOENT, "expected -ENOENT, got %d (val=%llu)", rc, val);

    str = "foo\0\\000\0\0 0,11,,22,,333432432,,,";
    rc = niova_parse_comma_delimited_uint_string(str, strlen(str), &val);
    MY_FATAL_IF(rc != -ENOENT, "expected -ENOENT, got %d (val=%llu)", rc, val);

    str = "0,";
    rc = niova_parse_comma_delimited_uint_string(str, strlen(str), &val);
    MY_FATAL_IF(rc != -ENOENT, "expected -ENOENT, got %d (val=%llu)", rc, val);

    str = "foo 10,000,000, bar";
    rc = niova_parse_comma_delimited_uint_string(str, strlen(str), &val);
    MY_FATAL_IF(rc != -ENOENT, "expected -ENOENT, got %d (val=%llu)", rc, val);

    str = "100,000,000,000,000";
    rc = niova_parse_comma_delimited_uint_string(str, strlen(str), &val);
    MY_FATAL_IF(rc != 0 || val != 100000000000000,
                "expected 0, got %d (val=%llu)", rc, val);

    str = "100,000,000,000,00";
    rc = niova_parse_comma_delimited_uint_string(str, strlen(str), &val);
    MY_FATAL_IF(rc != -ENOENT, "expected -ENOENT, got %d (val=%llu)", rc, val);

    str = "\t\n 100,000,000,999,000";
    rc = niova_parse_comma_delimited_uint_string(str, strlen(str), &val);
    MY_FATAL_IF(rc != 0 || val != 100000000999000,
                "expected 0, got %d (val=%llu)", rc, val);

    str = "100,000,000,000,999 ";
    rc = niova_parse_comma_delimited_uint_string(str, strlen(str), &val);
    MY_FATAL_IF(rc != 0 || val != 100000000000999,
                "expected 0, got %d (val=%llu)", rc, val);

}

struct b_type
{
    int b;
};

struct a_type
{
    struct b_type a_b_first;
    struct b_type a_b_array[10];
    struct b_type a_b_last;
};

static void
util_offset_cast_test(void)
{
    struct a_type a;
    struct b_type *b = &a.a_b_last;

    struct a_type *a_ptr = OFFSET_CAST(a_type, a_b_last, b);
    MY_FATAL_IF(a_ptr != &a, "expected %p got %p", &a, a_ptr);

    b = &a.a_b_first;
    a_ptr = OFFSET_CAST(a_type, a_b_first, b);
    MY_FATAL_IF(a_ptr != &a, "expected %p got %p", &a, a_ptr);

    for (int i = 0; i < 10; i++)
    {
        b = &a.a_b_array[i];
        a_ptr = OFFSET_CAST(a_type, a_b_array[i], b);
        MY_FATAL_IF(a_ptr != &a, "expected %p got %p", &a, a_ptr);
    }
}

static void
niova_crc_test(void)
{
    struct xx {
        int header_ignore;
        crc32_t obj_crc;
        int a[100];
        int b[];
    };

    struct xx crc_test = {0};

    crc32_t crc = NIOVA_CRC_OBJ(&crc_test, struct xx, obj_crc, 0);
    NIOVA_ASSERT(crc == crc_test.obj_crc);

    // NIOVA_CRC_OBJ_VERIFY() is effectively equivalent to the check above
    NIOVA_ASSERT(NIOVA_CRC_OBJ_VERIFY(&crc_test, struct xx, obj_crc, 0) == 0);

    // Recalculate the crc and assert the ignored data has not altered the crc
    crc_test.header_ignore = 1;
    NIOVA_CRC_OBJ(&crc_test, struct xx, obj_crc, 0);
    NIOVA_ASSERT(crc == crc_test.obj_crc);

    NIOVA_ASSERT(NIOVA_CRC_OBJ_VERIFY(&crc_test, struct xx, obj_crc, 0) == 0);

    // Invalidate the CRC and ensure that NIOVA_CRC_OBJ_VERIFY() fails
    crc_test.a[0] = 1;
    NIOVA_ASSERT(NIOVA_CRC_OBJ_VERIFY(&crc_test, struct xx, obj_crc, 0) != 0);

    // Test w/ trailing data

    char xbuf[sizeof(struct xx) * 2] = {0};

    struct xx *crc_test_extra = (struct xx *)xbuf;

    crc = NIOVA_CRC_OBJ(crc_test_extra, struct xx, obj_crc, sizeof(struct xx));
    NIOVA_ASSERT(crc == crc_test_extra->obj_crc);
    NIOVA_ASSERT(
        NIOVA_CRC_OBJ_VERIFY(crc_test_extra, struct xx, obj_crc,
                             sizeof(struct xx)) == 0);

    xbuf[sizeof(struct xx) + 1] = 1;

    NIOVA_ASSERT(
        NIOVA_CRC_OBJ_VERIFY(crc_test_extra, struct xx, obj_crc,
                             sizeof(struct xx)) != 0);
}

static void
niova_crc32_continuation_test(void)
{
    uint32_t buf[1000];

    for (int i = 0; i < 1000; i++)
        buf[i] = random_get();

    const crc32_t x = niova_crc((unsigned char *)buf, 1000 * sizeof(int), 0);
    SIMPLE_LOG_MSG(LL_DEBUG, "0x%08x", x);

    crc32_t y = 0;

    for (int i = 0; i < 1000; i++)
        y = niova_crc((unsigned char *)&buf[i], sizeof(int), y);

    NIOVA_ASSERT(y == x);

    y = 0;
    for (int i = 0; i < 10; i++)
        y = niova_crc((unsigned char *)&buf[i * 100], 100 * sizeof(int), y);

    NIOVA_ASSERT(y == x);
}

static void
niova_crc16_continuation_test(void)
{
    uint32_t buf[501];

    for (int i = 0; i < 500; i++)
        buf[i] = random_get();

    const uint16_t x = niova_t10dif_crc(0, (unsigned char *)buf,
                                        500 * sizeof(int));
    SIMPLE_LOG_MSG(LL_DEBUG, "t10dif 0x%04x", x);

    uint16_t y = 0;

    y = 0;
    for (int i = 0; i < 10; i++)
        y = niova_t10dif_crc(y, (unsigned char *)&buf[i * 50],
                             50 * sizeof(int));
    NIOVA_ASSERT(y == x);

    y = 0;
    y = niova_t10dif_crc(y, (unsigned char *)buf, 250 * sizeof(int));
    y = niova_t10dif_crc(y, (unsigned char *)&buf[250], 150 * sizeof(int));
    y = niova_t10dif_crc(y, (unsigned char *)&buf[400], 80 * sizeof(int));
    y = niova_t10dif_crc(y, (unsigned char *)&buf[480], 20 * sizeof(int));

    NIOVA_ASSERT(y == x);

    //Testing with unaligned access
    y = 0;
    unsigned char *unaligned_buf = ((unsigned char *)buf) + 1;
    memmove(unaligned_buf, buf, 500 * sizeof(int));
    y = niova_t10dif_crc(0, unaligned_buf, 500 * sizeof(int));
    NIOVA_ASSERT(y == x);

}

int
main(void)
{
    util_offset_cast_test();
    mk_time_string_test();
    niova_string_to_unsigned_long_long_test();
    niova_string_to_long_long_test();
    niova_string_to_unsigned_int_test();
    niova_parse_comma_delimited_uint_string_test();
    niova_crc_test();
    niova_crc32_continuation_test();
    niova_crc16_continuation_test();
    return 0;
}
