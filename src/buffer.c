/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Paul Nowoczynski <pauln@niova.io> 2021
 */

#include <sys/uio.h>
#include <unistd.h>

#include "common.h"

#include "alloc.h"
#include "buffer.h"
#include "log.h"
#include "registry.h"

static size_t bufferSetPageSize;
static size_t bufferSetPageBits;

REGISTRY_ENTRY_FILE_GENERATE;

LREG_ROOT_ENTRY_GENERATE(buffer_set_nodes, LREG_USER_TYPE_BUFFER_SET);

enum buffer_set_lreg_stats
{
    BUFFER_SET_NAME,              // string
    BUFFER_SET_LREG_ITEM_SZ,      // unsigned int
    BUFFER_SET_LREG_NUM_BUFS,     // signed int
    BUFFER_SET_LREG_OUTSTANDING,  // signed int
    BUFFER_SET_LREG_PNDG,          // signed int
    BUFFER_SET_LREG_TOTAL_ALLOCS, // unsigned int
    BUFFER_SET_LREG_MAX_USED,     // signed int
    BUFFER_SET_LREG_USER_CACHED,  // signed int
    BUFFER_SET_LREG___MAX,
};

static int
buffer_lreg_cb(enum lreg_node_cb_ops op, struct lreg_node *lrn,
               struct lreg_value *lv)
{
    const struct buffer_set *bs = lrn->lrn_cb_arg;
    if (!bs)
        return -EINVAL;

    int rc = 0;

    switch (op)
    {
    case LREG_NODE_CB_OP_WRITE_VAL:           // fall through
    case LREG_NODE_CB_OP_INSTALL_QUEUED_NODE: // fall through
    case LREG_NODE_CB_OP_INSTALL_NODE:        // fall through
    case LREG_NODE_CB_OP_DESTROY_NODE:
        break;

    case LREG_NODE_CB_OP_GET_NAME:
        if (!lv)
            return -EINVAL;
        lv->get.lrv_num_keys_out = BUFFER_SET_LREG___MAX;
        strncpy(lv->lrv_key_string, "buffer-sets", LREG_VALUE_STRING_MAX);
        break;

    case LREG_NODE_CB_OP_READ_VAL:
        switch (lv->lrv_value_idx_in)
        {
        case BUFFER_SET_NAME:
            lreg_value_fill_string(lv, "name", bs->bs_name);
            break;
        case BUFFER_SET_LREG_ITEM_SZ:
            lreg_value_fill_unsigned(lv, "buf-size", bs->bs_item_size);
            break;
        case BUFFER_SET_LREG_NUM_BUFS:
            lreg_value_fill_signed(lv, "num-bufs", bs->bs_num_bufs);
            break;
        case BUFFER_SET_LREG_OUTSTANDING:
            lreg_value_fill_signed(lv, "in-use", bs->bs_num_allocated);
            break;
        case BUFFER_SET_LREG_PNDG:
            lreg_value_fill_unsigned(lv, "pdng-alloc", bs->bs_num_pndg_alloc);
            break;
        case BUFFER_SET_LREG_TOTAL_ALLOCS:
            lreg_value_fill_unsigned(lv, "total-used", bs->bs_total_alloc);
            break;
        case BUFFER_SET_LREG_MAX_USED:
            lreg_value_fill_signed(lv, "max-in-use", bs->bs_max_allocated);
            break;
        case BUFFER_SET_LREG_USER_CACHED:
            lreg_value_fill_signed(lv, "num-user-cached",
                                   bs->bs_num_user_cached);
            break;
        };
        break;

    default:
        rc = -ENOENT;
        break;
    }

    return rc;
}

/* Use cast to override 'const *bs' which is needed for at least one public
 * routine.
 */
#define BS_LOCK(bs)                                                  \
    do {                                                             \
        if ((bs)->bs_serialize)                                      \
            niova_mutex_lock((pthread_mutex_t *)&(bs)->bs_mutex);    \
    } while (0)

#define BS_UNLOCK(bs)                                                   \
    do {                                                                \
        if ((bs)->bs_serialize)                                         \
            niova_mutex_unlock((pthread_mutex_t *)&(bs)->bs_mutex);     \
    } while (0)

static void
buffer_page_size_set(void)
{
    if (bufferSetPageSize)
        return;

    bufferSetPageSize = sysconf(_SC_PAGESIZE);
    NIOVA_ASSERT(bufferSetPageSize == 4096);

    bufferSetPageBits = highest_set_bit_pos_from_val(bufferSetPageSize) - 1;
    NIOVA_ASSERT(bufferSetPageBits == 12);
}

unsigned int
buffer_get_alignment(enum buffer_set_opts opts)
{
    // Start from the largest and work downwards
    if (opts & BUFSET_OPT_MEMALIGN_SECTOR || opts & BUFSET_OPT_MEMALIGN)
        return BUFFER_SECTOR_SIZE;

    if (opts & BUFSET_OPT_MEMALIGN_L2)
        return L2_CACHELINE_SIZE_BYTES;

    return 0; // No alignment flag was detected
}

size_t
buffer_page_size(void)
{
    return bufferSetPageSize;
}

size_t
buffer_set_size_to_nblks(const struct buffer_set *bs)
{
    return bs ? bs->bs_item_size >> bufferSetPageBits : 0;
}

static size_t
buffer_set_navail_locked(const struct buffer_set *bs)
{
    NIOVA_ASSERT(bs);

    NIOVA_ASSERT(bs->bs_num_allocated  >= 0);
    NIOVA_ASSERT(bs->bs_num_pndg_alloc >= 0);
    NIOVA_ASSERT((ssize_t)bs->bs_num_bufs >=
                 (bs->bs_num_allocated + bs->bs_num_pndg_alloc));

    return bs->bs_num_bufs - (bs->bs_num_allocated + bs->bs_num_pndg_alloc);
}

size_t
buffer_set_navail(const struct buffer_set *bs)
{
    if (!bs)
        return 0;

    BS_LOCK(bs);
    size_t navail = buffer_set_navail_locked(bs);
    BS_UNLOCK(bs);

    return navail;
}

static void
buffer_item_release_init(struct buffer_item *bi);

static void
buffer_item_user_cache_revoke(struct buffer_item *bi)
{
    if (bi->bi_user_cached) // execute the callback
    {
        NIOVA_ASSERT(bi->bi_cache_revoke_cb != NULL);

        bi->bi_cache_revoke_cb(bi, bi->bi_cache_revoke_arg);

        struct buffer_set *bs = bi->bi_bs;
        NIOVA_ASSERT(bs->bs_num_user_cached > 0);
        bs->bs_num_user_cached--;

        buffer_item_release_init(bi);
    }
}

static struct buffer_item *
buffer_set_allocate_item_locked(struct buffer_set *bs)
{
    NIOVA_ASSERT(bs);

    size_t navail = buffer_set_navail_locked(bs);

    if (!navail)
    {
        NIOVA_ASSERT(CIRCLEQ_EMPTY(&bs->bs_free_list));

        return NULL;
    }

    NIOVA_ASSERT(!CIRCLEQ_EMPTY(&bs->bs_free_list));

    // Allocate from the head
    struct buffer_item *bi = CIRCLEQ_FIRST(&bs->bs_free_list);

    if (bi->bi_user_cached) // execute the callback
    {
        NIOVA_ASSERT(bs->bs_allow_user_cache);
        buffer_item_user_cache_revoke(bi);
    }

    CIRCLEQ_REMOVE(&bs->bs_free_list, bi, bi_lentry);
    CIRCLEQ_INSERT_TAIL(&bs->bs_inuse_list, bi, bi_lentry);

    bs->bs_total_alloc++;

    bs->bs_num_allocated++;
    bi->bi_allocated = true;

    if (bs->bs_num_allocated > (ssize_t)bs->bs_max_allocated)
        bs->bs_max_allocated = bs->bs_num_allocated;

    return bi;
}

struct buffer_item *
buffer_set_allocate_item(struct buffer_set *bs)
{
    if (!bs)
        return NULL;

    BS_LOCK(bs);

    struct buffer_item *bi = buffer_set_allocate_item_locked(bs);

    BS_UNLOCK(bs);

    return bi;
}

struct buffer_item *
buffer_set_allocate_item_from_pending(struct buffer_set *bs)
{
    NIOVA_ASSERT(bs);
    NIOVA_ASSERT(bs->bs_num_pndg_alloc > 0);

    BS_LOCK(bs);

    bs->bs_num_pndg_alloc--;
    struct buffer_item *bi = buffer_set_allocate_item_locked(bs);

    NIOVA_ASSERT(bi);

    BS_UNLOCK(bs);

    return bi;
}

int
buffer_set_release_pending_alloc(struct buffer_set *bs, const size_t nitems)
{
    if (!bs || nitems > bs->bs_num_bufs)
        return -EINVAL;

    BS_LOCK(bs);

    if ((ssize_t)nitems > bs->bs_num_pndg_alloc)
    {
        BS_UNLOCK(bs);

        return -EOVERFLOW;
    }

    bs->bs_num_pndg_alloc -= nitems;

    BS_UNLOCK(bs);

    return 0;
}

int
buffer_set_pending_alloc(struct buffer_set *bs, const size_t nitems)
{
    if (!bs || !nitems)
        return -EINVAL;

    BS_LOCK(bs);

    if (bs->bs_num_bufs < nitems)
    {
        BS_UNLOCK(bs);
        return -ENOMEM;
    }
    else if (buffer_set_navail_locked(bs) < nitems)
    {
        BS_UNLOCK(bs);
        return -ENOBUFS;
    }

    bs->bs_num_pndg_alloc += nitems;

    BS_UNLOCK(bs);

    return 0;
}

static void
buffer_item_release_init(struct buffer_item *bi)
{
    bi->bi_iov = bi->bi_iov_save;
    bi->bi_allocated = false;
    bi->bi_cache_revoke_cb = NULL;
    bi->bi_user_cached = 0;

    SLIST_ENTRY_INIT(&bi->bi_user_slentry);
}

void
buffer_set_release_item(struct buffer_item *bi)
{
    if (!bi)
        return;

    struct buffer_set *bs = bi->bi_bs;
    NIOVA_ASSERT(bs);

    BS_LOCK(bs);

    if (bi->bi_user_cached)
    {
        NIOVA_ASSERT(!bi->bi_allocated);

        NIOVA_ASSERT(bs->bs_num_user_cached > 0);
        bs->bs_num_user_cached--;

        // Note that item was already added to the free list
    }
    else
    {
        NIOVA_ASSERT(bi->bi_allocated);
        NIOVA_ASSERT(bs->bs_num_allocated > 0);
        bs->bs_num_allocated--;

        CIRCLEQ_REMOVE(&bs->bs_inuse_list, bi, bi_lentry);

        // Release to the head since item is not user cached
        CIRCLEQ_INSERT_HEAD(&bs->bs_free_list, bi, bi_lentry);
    }

    buffer_item_release_init(bi);

    BS_UNLOCK(bs);
}

/**
 * buffer_set_user_cache_release_item - enacts a "partial" release on the item
 *    placing it into the free list but promises to execute the provided
 *    callback prior to allocation the item to another user.
 */
int
buffer_set_user_cache_release_item(
    struct buffer_item *bi, void (*revoke_cb)(struct buffer_item *, void *),
    void *arg)
{
    if (bi == NULL || revoke_cb == NULL)
        return -EINVAL;

    if (bi->bi_user_cached)
        return -EALREADY;

    struct buffer_set *bs = bi->bi_bs;
    NIOVA_ASSERT(bs);

    if (!bs->bs_allow_user_cache)
        return -EPERM;

    bi->bi_cache_revoke_cb = revoke_cb;
    bi->bi_cache_revoke_arg = arg;
    bi->bi_user_cached = 1;

    BS_LOCK(bs);

    NIOVA_ASSERT(bi->bi_allocated);
    NIOVA_ASSERT(bs->bs_num_allocated > 0);
    bi->bi_allocated = false;
    bs->bs_num_allocated--;

    bs->bs_num_user_cached++;
    NIOVA_ASSERT(bs->bs_num_user_cached > 0);

    CIRCLEQ_REMOVE(&bs->bs_inuse_list, bi, bi_lentry);

    // 'LRU' by placing at the tail (allocation occurs from head)
    CIRCLEQ_INSERT_TAIL(&bs->bs_free_list, bi, bi_lentry);

    BS_UNLOCK(bs);

    return 0;
}

int
buffer_set_destroy(struct buffer_set *bs)
{
    if (!bs)
        return -EINVAL;

    else if (!bs->bs_init)
        return -EALREADY;

    else if (bs->bs_num_allocated)
        return -EBUSY;

    NIOVA_ASSERT(!bs->bs_num_allocated);

    struct buffer_item *bi, *tmp;

    CIRCLEQ_FOREACH_SAFE(bi, &bs->bs_free_list, bi_lentry, tmp)
    {
        if (bi->bi_user_cached)
            buffer_item_user_cache_revoke(bi);

        CIRCLEQ_REMOVE(&bs->bs_free_list, bi, bi_lentry);

        free(bi);

        bs->bs_num_bufs--;
    }

    NIOVA_ASSERT(!bs->bs_num_bufs);

    bs->bs_region = NULL;
    bs->bs_init = 0;

    if (bs->bs_serialize)
        pthread_mutex_destroy(&bs->bs_mutex);

    if (bs->bs_ctl_interface)
    {
        NIOVA_ASSERT(lreg_node_is_installed(&bs->bs_lrn));

        int rc = lreg_node_remove(&bs->bs_lrn,
                                  LREG_ROOT_ENTRY_PTR(buffer_set_nodes));
        NIOVA_ASSERT(rc == 0);

        // Ensure removal before returning
        rc = lreg_node_wait_for_completion(&bs->bs_lrn, false);
        NIOVA_ASSERT(rc == 0);

        bs->bs_ctl_interface = 0;
    }

    return 0;
}

int
buffer_set_initx(struct buffer_set_args *bsa)
{
    struct buffer_set *bs = NULL;
    size_t prologue_size = 0;
    size_t s_region_size = 0;
    void *s_region = NULL;
    enum buffer_set_opts opts = 0;
    size_t nbufs = 0;
    size_t buf_size = 0;
    int err_loc = 0;
    int rc = 0;

    buffer_page_size_set();

    if (!bsa)
    {
        rc = -EINVAL;
        err_loc = 1;
        goto xerror;
    }

    opts = bsa->bsa_opts;
    prologue_size = bsa->bsa_prologue_size;
    buf_size = bsa->bsa_buf_size;
    nbufs = bsa->bsa_nbufs;
    s_region_size = bsa->bsa_region_size;
    s_region = bsa->bsa_region;
    bs = bsa->bsa_set;

    if (!bs || !buf_size || !s_region || !s_region_size || bs->bs_init)
    {
        rc = -EINVAL;
        err_loc = 2;
        goto xerror;
    }

    if (bsa->bsa_used_off != 0)
    {
        rc = -EINVAL;
        err_loc = 3;
        goto xerror;
    }

    if (s_region_size < (nbufs * (buf_size + prologue_size)))
    {
        rc = -EOVERFLOW;
        err_loc = 4;
        goto xerror;
    }

    unsigned int align = bsa->bsa_alignment ?
        bsa->bsa_alignment : buffer_get_alignment(bsa->bsa_opts);

    /* Alignment should be power of 2 */
    if (align && !IS_POWER2(align))
    {
        rc = -EDOM;
        err_loc = 5;
        goto xerror;
    }

    if (align && !IS_ALIGNED(buf_size, align))
    {
        rc = -EDOM;
        err_loc = 6;
        goto xerror;
    }

    if (align && prologue_size && !IS_ALIGNED(prologue_size, align))
    {
        rc = -EDOM;
        err_loc = 7;
        goto xerror;
    }

    if (align && !IS_ALIGNED_PTR(s_region, align))
    {
        rc = -EFAULT;
        err_loc = 8;
        goto xerror;
    }

    NIOVA_ASSERT(!align || IS_ALIGNED_PTR(s_region, align));
    NIOVA_ASSERT(!align || IS_ALIGNED(s_region_size, align));

    memset(bs, 0, sizeof(struct buffer_set));

    bs->bs_region = s_region;
    bs->bs_region_size = s_region_size;
    bs->bs_item_size = buf_size;
    bs->bs_num_bufs = 0;
    CIRCLEQ_INIT(&bs->bs_free_list);
    CIRCLEQ_INIT(&bs->bs_inuse_list);

    if (opts & BUFSET_OPT_USER_CACHE)
    {
        bs->bs_allow_user_cache = 1;
    }

    if (opts & BUFSET_OPT_SERIALIZE)
    {
        bs->bs_serialize = 1;
        pthread_mutex_init(&bs->bs_mutex, NULL);
    }

    SIMPLE_LOG_MSG(LL_NOTIFY, "s_region %p s_region_end 0x%lx size %ld "
                   "prlg_sz %ld bf_sz %ld bf_cnt %ld alignment %u",
                   s_region, (uintptr_t)((char *)s_region + s_region_size),
                   s_region_size, prologue_size, buf_size, nbufs, align);

    size_t off = 0;
    uintptr_t prologue_start = (uintptr_t)NULL;
    uintptr_t prev_end = (uintptr_t)NULL;

    for (size_t i = 0; i < nbufs; i++)
    {
        struct buffer_item *bi = calloc(1, sizeof(struct buffer_item));
        if (!bi)
        {
            rc = -ENOMEM;
            err_loc = 9;
            goto xerror;
        }

        bi->bi_bs = bs;
        bi->bi_iov.iov_len = buf_size;
        bi->bi_register_idx = -1;

        NIOVA_ASSERT(off + buf_size + prologue_size <= s_region_size);


        /* |<---hidden---->|<------exposed------>|
         * |----------contiguous region----------|
         * |---------------|---------------------|
         * |<--prologue--->|<-----iov_base------>|
         */

        prologue_start = (uintptr_t)((char *)s_region + off);

        char *iov_base = (char *)s_region + off + prologue_size;
        bi->bi_iov.iov_base = iov_base;

        /* Rule out buffer/prologue address overlap */
        NIOVA_ASSERT(!prev_end || prologue_start >= prev_end);

        NIOVA_ASSERT(
            (uintptr_t)iov_base == (prologue_start + (uintptr_t)prologue_size));

        prev_end =
            (uintptr_t)((char *)bi->bi_iov.iov_base + bi->bi_iov.iov_len);

        off += buf_size + prologue_size;

        SIMPLE_LOG_MSG(LL_NOTIFY,
            "prlg_sz %ld bf_sz %ld base[%ld] 0x%lx end[%ld] 0x%lx alignment %u",
            prologue_size, buf_size, i,
            (uintptr_t)(bi->bi_iov.iov_base), i, prev_end, align);

        FATAL_IF(align && prologue_size &&
                 (!IS_ALIGNED_PTR(prologue_start, align)),
                 "prologue start not aligned!");

        FATAL_IF(align && (!IS_ALIGNED_PTR(bi->bi_iov.iov_base, align)),
                 "iov_base not aligned!");

        NIOVA_ASSERT(off <= s_region_size);
        CONST_OVERRIDE(struct iovec, bi->bi_iov_save, bi->bi_iov);

        CIRCLEQ_INSERT_HEAD(&bs->bs_free_list, bi, bi_lentry);
        bs->bs_num_bufs++;

        if (!(opts & BUFSET_OPT_MMAP_REGION))
            buffer_item_touch(bi);
    }
    bsa->bsa_used_off = off;
    NIOVA_ASSERT(bsa->bsa_used_off <= s_region_size);

    if (opts & BUFSET_OPT_LREG)
    {
        lreg_node_init(&bs->bs_lrn, LREG_USER_TYPE_BUFFER_SET,
                       buffer_lreg_cb, bs, LREG_INIT_OPT_NONE);

        rc = lreg_node_install(&bs->bs_lrn,
                               LREG_ROOT_ENTRY_PTR(buffer_set_nodes));
        NIOVA_ASSERT(rc == 0);

        rc = lreg_node_wait_for_completion(&bs->bs_lrn, true);
        NIOVA_ASSERT(rc == 0);

        bs->bs_ctl_interface = 1;
    }

    bs->bs_init = true;

xerror:
    if (rc)
        buffer_set_destroy(bs);

    enum log_level log_lvl = rc ? LL_ERROR : LL_DEBUG;
    SIMPLE_LOG_MSG(log_lvl, "region %p region_sz %ld nbufs %ld bf_sz %ld "
                   "err_loc: %d rc %d",
                   s_region, s_region_size, nbufs, buf_size, err_loc, rc);
    return rc;
}

int
buffer_set_init(struct buffer_set *bs, void *region, size_t region_size,
                size_t nbufs, size_t buf_size, enum buffer_set_opts opts)
{
    struct buffer_set_args bsa = {
        .bsa_set = bs,
        .bsa_nbufs = nbufs,
        .bsa_buf_size = buf_size,
        .bsa_region = region,
        .bsa_region_size = region_size,
        .bsa_opts = opts,
    };

    return buffer_set_initx(&bsa);
}

int
buffer_set_apply_name(struct buffer_set *bs, const char *name)
{
    if (bs == NULL || name == NULL)
        return -EINVAL;

    strncpy(bs->bs_name, name, BUFFER_SET_NAME_MAX);

    return 0;
}

static init_ctx_t NIOVA_CONSTRUCTOR(BUFFER_SET_CTOR_PRIORITY)
buffer_set_ctor(void)
{
    LREG_ROOT_ENTRY_INSTALL(buffer_set_nodes);

    return;
}
