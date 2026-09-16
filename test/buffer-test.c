/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Paul Nowoczynski <pauln@niova.io> 2021
 */

#include "buffer.h"
#include "log.h"

static void
buffer_test(bool serialize, bool lreg)
{
    struct buffer_set bs = {0};

    enum buffer_set_opts opts =
        (serialize ? BUFSET_OPT_SERIALIZE : 0) |
        (lreg ? BUFSET_OPT_LREG : 0);

    int rc = buffer_set_init(NULL, NULL, 0, 0, 0, opts);
    NIOVA_ASSERT(rc == -EINVAL);

    // page size is set on first call to set_init()
    NIOVA_ASSERT(buffer_page_size() == 4096);

    rc = buffer_set_init(&bs, NULL, 0, 0, 0, opts);
    NIOVA_ASSERT(rc == -EINVAL);

    char buf[10];
    // init and destroy 'bs'
    rc = buffer_set_init(&bs, &buf[0], 1, 1, 1, opts);

    NIOVA_ASSERT(rc == 0);
    NIOVA_ASSERT(buffer_set_navail(&bs) == 1);

    struct buffer_item *bi = buffer_set_allocate_item(&bs);
    NIOVA_ASSERT(bi);
    NIOVA_ASSERT(buffer_set_navail(&bs) == 0);

    rc = buffer_set_destroy(NULL);
    NIOVA_ASSERT(rc == -EINVAL);
    rc = buffer_set_destroy(&bs);
    NIOVA_ASSERT(rc == -EBUSY);

    buffer_set_release_item(bi);
    NIOVA_ASSERT(buffer_set_navail(&bs) == 1);
    rc = buffer_set_destroy(&bs);
    NIOVA_ASSERT(rc == 0);

    // reserved ops
    const size_t n = 10;
    rc = buffer_set_init(&bs, &buf[0], 10, n, 1, opts);

    NIOVA_ASSERT(rc == 0);
    NIOVA_ASSERT(buffer_set_navail(&bs) == n);

    rc = buffer_set_pending_alloc(&bs, n + 1);
    NIOVA_ASSERT(rc == -ENOMEM); // this will never succeed

    rc = buffer_set_pending_alloc(&bs, n - 1);
    NIOVA_ASSERT(rc == 0);

    rc = buffer_set_pending_alloc(&bs, 1 + (n - (n - 1)));
    NIOVA_ASSERT(rc == -ENOBUFS); // ok once 2 or buffers are available

    struct buffer_item *bi_array[n - 1];

    for (unsigned int i = 0; i < (n - 1); i++)
    {
        bi_array[i] = buffer_set_allocate_item_from_pending(&bs);
        NIOVA_ASSERT(bi_array[i]);
    }
    // should be one left..
    NIOVA_ASSERT(buffer_set_navail(&bs) == 1);

    rc = buffer_set_destroy(&bs);
    NIOVA_ASSERT(rc == -EBUSY);

    for (unsigned int i = 0; i < (n - 1); i++)
        buffer_set_release_item(bi_array[i]);

    rc = buffer_set_destroy(&bs);
    NIOVA_ASSERT(rc == 0);
}

static void
buffer_user_cache_test_cb(struct buffer_item *bi, void *arg)
{
    NIOVA_ASSERT(bi && bi->bi_user_cached && arg == bi->bi_cache_revoke_arg);

    struct buffer_item **item = (struct buffer_item **)arg;

    NIOVA_ASSERT(*item == bi);
    *item = NULL;
}

static void
buffer_user_cache_test(void)
{
    struct buffer_set bs;

#define BUCT_BUFSZ 128
#define BUCT_NBUFS 2

    char buf[BUCT_BUFSZ];
    size_t buf_sz = 64;
    struct buffer_item *items[BUCT_NBUFS];

    /* 1. Try the release call w/out setting BUFSET_OPT_USER_CACHE
     */
    int rc = buffer_set_init(&bs, &buf[0], BUCT_BUFSZ, BUCT_NBUFS, buf_sz, 0);
    NIOVA_ASSERT(rc == 0);

    struct buffer_item *bi = buffer_set_allocate_item(&bs);
    NIOVA_ASSERT(bi);
    NIOVA_ASSERT(!bi->bi_user_cached);

    rc = buffer_set_user_cache_release_item(bi, NULL, NULL);
    NIOVA_ASSERT(rc == -EINVAL);

    rc = buffer_set_user_cache_release_item(bi, buffer_user_cache_test_cb,
                                            NULL);
    NIOVA_ASSERT(rc == -EPERM);

    buffer_set_release_item(bi);

    rc = buffer_set_destroy(&bs);
    NIOVA_ASSERT(rc == 0);

    /* 2. Setup buffer_set w/ BUFSET_OPT_USER_CACHE w/ a single item
     */
    rc = buffer_set_init(&bs, &buf[0], BUCT_BUFSZ, 1, buf_sz,
                         BUFSET_OPT_USER_CACHE);
    NIOVA_ASSERT(rc == 0);

    // 2a. allocate item and perform a typical release
    bi = buffer_set_allocate_item(&bs);
    NIOVA_ASSERT(bi);
    NIOVA_ASSERT(!bi->bi_user_cached);

    buffer_set_release_item(bi);

    NIOVA_ASSERT(bs.bs_num_user_cached == 0);

    //
    // 2b. allocate item, make it eligible for caching, then explicitly release
    bi = buffer_set_allocate_item(&bs);
    NIOVA_ASSERT(bi);
    NIOVA_ASSERT(!bi->bi_user_cached);

    rc = buffer_set_user_cache_release_item(bi, buffer_user_cache_test_cb,
                                            &items[0]);
    NIOVA_ASSERT(rc == 0);
    NIOVA_ASSERT(bi);
    NIOVA_ASSERT(bi->bi_user_cached);
    NIOVA_ASSERT(bs.bs_num_user_cached == 1);

    buffer_set_release_item(bi);
    items[0] = bi;
    NIOVA_ASSERT(bs.bs_num_user_cached == 0);
    NIOVA_ASSERT(items[0] == bi); // callback should *not* have run
    items[0] = NULL;

    //
    // 2c. Observe item being removed via callback
    bi = buffer_set_allocate_item(&bs);
    NIOVA_ASSERT(bi);
    NIOVA_ASSERT(!bi->bi_user_cached);
    items[0] = bi;

    rc = buffer_set_user_cache_release_item(bi, buffer_user_cache_test_cb,
                                            &items[0]);
    NIOVA_ASSERT(rc == 0);
    NIOVA_ASSERT(bi);
    NIOVA_ASSERT(bi->bi_user_cached);
    NIOVA_ASSERT(bs.bs_num_user_cached == 1);

    // 2c.1 Second allocation will cause the callback to be issued
    struct buffer_item *bi_new = buffer_set_allocate_item(&bs);
    NIOVA_ASSERT(bi);
    NIOVA_ASSERT(!bi->bi_user_cached);
    NIOVA_ASSERT(items[0] == NULL); // ensure the first item was removed

    items[0] = bi_new;
    // 2c.2 Cache the new item and observe it being released via set_destroy()
    rc = buffer_set_user_cache_release_item(bi_new, buffer_user_cache_test_cb,
                                            &items[0]);
    NIOVA_ASSERT(rc == 0);
    NIOVA_ASSERT(bi);
    NIOVA_ASSERT(bi->bi_user_cached);
    NIOVA_ASSERT(bs.bs_num_user_cached == 1);

    rc = buffer_set_destroy(&bs); // destroy w/ cached item
    NIOVA_ASSERT(rc == 0);

    NIOVA_ASSERT(items[0] == NULL); // callback must have run on destroy

    /* 3. Setup buffer_set w/ BUFSET_OPT_USER_CACHE w/ multiple items
     */
    rc = buffer_set_init(&bs, &buf[0], BUCT_BUFSZ,BUCT_NBUFS, buf_sz,
                         BUFSET_OPT_USER_CACHE);
    NIOVA_ASSERT(rc == 0);

    bi = items[0] = buffer_set_allocate_item(&bs);
    // 3a Cache item
    rc = buffer_set_user_cache_release_item(bi, buffer_user_cache_test_cb,
                                            &items[0]);
    NIOVA_ASSERT(rc == 0);
    NIOVA_ASSERT(bi);
    NIOVA_ASSERT(bi->bi_user_cached);
    NIOVA_ASSERT(bs.bs_num_user_cached == 1);

    // 3b. Allocate a second item and ensure that the first was not release
    for (int i = 0; i < 10; i++)
    {
        bi = buffer_set_allocate_item(&bs);
        NIOVA_ASSERT(rc == 0);
        NIOVA_ASSERT(items[0] != NULL); // ensure cached item remains
        NIOVA_ASSERT(items[0]->bi_user_cached);
        buffer_set_release_item(bi);
    }

    bi = items[0];
    buffer_set_release_item(items[0]);
    NIOVA_ASSERT(items[0] == bi); // callback should not have been issued

    buffer_set_destroy(&bs);
    NIOVA_ASSERT(items[0] == bi); // callback should not have been issued
}

static void
buffer_initx_test(void)
{
    size_t size = 1024UL*1024UL;
    void *base = malloc(size);
    NIOVA_ASSERT(base != NULL);

    struct buffer_set bs = {0};

    struct buffer_set_args bsa =
    {
        .bsa_set = &bs,
        .bsa_nbufs = 1024UL,
        .bsa_buf_size = 1024UL,
        .bsa_region = NULL,
        .bsa_region_size = size,
        .bsa_alignment = 16,
    };

    int rc = buffer_set_initx(&bsa);
    NIOVA_ASSERT(rc == -EINVAL);

    bsa.bsa_region = base;
    bsa.bsa_region_size -= 1;

    rc = buffer_set_initx(&bsa);
    NIOVA_ASSERT(rc == -EOVERFLOW);

    bsa.bsa_region_size += 1;

    rc = buffer_set_initx(&bsa);
    NIOVA_ASSERT(rc == 0);

    NIOVA_ASSERT(bsa.bsa_used_off == size);

    struct buffer_item *bi = buffer_set_allocate_item(&bs);
    NIOVA_ASSERT(bi && bi->bi_iov.iov_len == 1024UL);
    NIOVA_ASSERT(((uintptr_t)bi->bi_iov.iov_base >= (uintptr_t)base) &&
                 ((uintptr_t)bi->bi_iov.iov_base < (((uintptr_t)base) + size)));

    buffer_set_release_item(bi);

    rc = buffer_set_destroy(&bs);
    NIOVA_ASSERT(rc == 0);

    free(base);
}

static void
buffer_initx_memalign_test(size_t nbufs, size_t buf_size,
                           unsigned int alignment,
                           enum buffer_set_opts opts)
{
    size_t buf_size_aligned = ALIGN_UP(buf_size, alignment);
    size_t region_size = ALIGN_UP(buf_size_aligned * nbufs, alignment);
    void *base = malloc(region_size);
    NIOVA_ASSERT(base != NULL);

    struct buffer_set bs = {0};

    struct buffer_set_args bsa =
    {
        .bsa_set = &bs,
        .bsa_opts = opts,
        .bsa_nbufs = nbufs,
        .bsa_buf_size = buf_size_aligned,
        .bsa_region = base,
        .bsa_region_size = region_size,
        .bsa_alignment = alignment,
    };

    uint8_t initialized = 1;
    //either base is aligned on boundary or it is not
    int rc = buffer_set_initx(&bsa);
    NIOVA_ASSERT(rc == 0 ||
                 (rc == -EFAULT && !IS_ALIGNED_PTR(base, alignment)));

    // if base was not aligned, free it and allocate by using memalign
    if (rc == -EFAULT)
    {
        free(base);
        base = NULL;
        initialized = 0;
    }

    if (!base)
    {
        rc = posix_memalign(&base, alignment, region_size);
        NIOVA_ASSERT(rc == 0 && base);
        bsa.bsa_region = base;
    }

    rc = !initialized ? buffer_set_initx(&bsa) : 0;
    NIOVA_ASSERT(rc == 0);

    NIOVA_ASSERT(bsa.bsa_used_off <= region_size);

    struct buffer_item **bi_array =
        (struct buffer_item **)calloc(nbufs, sizeof(struct buffer_item *));
    NIOVA_ASSERT(bi_array);

    uintptr_t curr_base = (uintptr_t)NULL;
    uintptr_t region_end = (uintptr_t)((char *)base + region_size);

    for (size_t i = 0; i < nbufs; i++)
    {
        struct buffer_item *bi = buffer_set_allocate_item(&bs);
        NIOVA_ASSERT(bi && bi->bi_iov.iov_len == buf_size_aligned);

        curr_base = (uintptr_t)(bi->bi_iov.iov_base);
        NIOVA_ASSERT(curr_base >= (uintptr_t)base && curr_base < region_end);
        NIOVA_ASSERT(IS_ALIGNED_PTR(curr_base, alignment));

        bi_array[i] = bi;
    }

    for (size_t i = 0; i < nbufs; i++)
    {
        buffer_set_release_item(bi_array[i]);
    }

    rc = buffer_set_destroy(&bs);
    NIOVA_ASSERT(rc == 0);

    free(bi_array);
    free(base);
}

static void
buffer_initx_trigger_memalign_tests()
{
    buffer_initx_memalign_test(1024, 768, 1024, 0);
    buffer_initx_memalign_test(1024, 1280, 2048, 0);
    buffer_initx_memalign_test(10, 768 * 1024, 1024 * 1024, 0);
    buffer_initx_memalign_test(100, 268, 64, BUFSET_OPT_MEMALIGN_L2);
}

int
main(void)
{
    NIOVA_ASSERT(buffer_page_size() == 0);

    buffer_test(false, false);
    buffer_test(true, false);

    buffer_test(false, true);

    buffer_initx_test();

    buffer_user_cache_test();
    buffer_initx_trigger_memalign_tests();

    return 0;
}
