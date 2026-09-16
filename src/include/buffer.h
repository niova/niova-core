/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Paul Nowoczynski <pauln@niova.io> 2021
 */
#ifndef _NIOVA_BUFFER_H
#define _NIOVA_BUFFER_H 1

#include <sys/uio.h>
#include <stdint.h>
#include <pthread.h>

#include "common.h"
#include "queue.h"
#include "ref_tree_proto.h"
#include "registry.h"

#define BUFFER_SECTOR_SIZE 512UL

#define BUFFSET_OPT_ALIGNMENT_FLAGS_MASK \
    (BUFSET_OPT_MEMALIGN_L2     |        \
     BUFSET_OPT_MEMALIGN_SECTOR |        \
     BUFSET_OPT_MEMALIGN)                \

enum buffer_set_opts
{
    BUFSET_OPT_SERIALIZE       = (1 << 0),
    BUFSET_OPT_USER_CACHE      = (1 << 1),
    BUFSET_OPT_LREG            = (1 << 2),
    BUFSET_OPT_MEMALIGN_L2     = (1 << 3),
    BUFSET_OPT_MEMALIGN_SECTOR = (1 << 4),
    BUFSET_OPT_MMAP_REGION     = (1 << 5),
    BUFSET_OPT_MEMALIGN        = BUFSET_OPT_MEMALIGN_SECTOR,
};

struct buffer_set;
struct buffer_item
{
    struct buffer_set           *bi_bs;
    struct iovec                 bi_iov; // may be modified by user
    const struct iovec           bi_iov_save;
    CIRCLEQ_ENTRY(buffer_item)   bi_lentry;
    SLIST_ENTRY(buffer_item)     bi_user_slentry;
    const char                  *bi_allocator_func;
    void                       (*bi_cache_revoke_cb)(struct buffer_item *,
                                                     void *);
    void                        *bi_cache_revoke_arg;
    unsigned int                 bi_alloc_lineno:31;
    unsigned int                 bi_allocated:1;
    unsigned int                 bi_user_cached:1;
    int                          bi_register_idx;
};

CIRCLEQ_HEAD(buffer_list, buffer_item);
SLIST_HEAD(buffer_user_slist, buffer_item);

#define BUFFER_SET_NAME_MAX 32

struct buffer_set_args
{
    struct buffer_set   *bsa_set;
    size_t               bsa_nbufs;
    size_t               bsa_buf_size;
    size_t               bsa_prologue_size;
    void                *bsa_region;
    size_t               bsa_region_size;
    size_t               bsa_used_off;
    unsigned int         bsa_alignment; //used by tests
    enum buffer_set_opts bsa_opts;
};

struct buffer_set
{
    char                bs_name[BUFFER_SET_NAME_MAX + 1];
    size_t              bs_num_bufs;
    ssize_t             bs_num_allocated;
    ssize_t             bs_num_user_cached;
    ssize_t             bs_num_pndg_alloc;
    size_t              bs_item_size;
    size_t              bs_max_allocated;
    size_t              bs_total_alloc;
    uint8_t             bs_init:1;
    uint8_t             bs_serialize:1;
    uint8_t             bs_ctl_interface:1;
    uint8_t             bs_allow_user_cache:1;
    struct buffer_list  bs_free_list;
    struct buffer_list  bs_inuse_list;
    size_t              bs_region_size;
    void               *bs_region;
    pthread_mutex_t     bs_mutex;
    struct lreg_node    bs_lrn;
};

size_t
buffer_page_size(void);

static inline void
buffer_item_touch(struct buffer_item *bi)
{
    if (!bi)
        return;

    char *x = (char *)bi->bi_iov.iov_base;

    for (size_t off = 0; off < bi->bi_iov.iov_len; off += buffer_page_size())
        x[off] = 0xff;
}

size_t
buffer_set_size_to_nblks(const struct buffer_set *bs);

size_t
buffer_set_navail(const struct buffer_set *bs);

struct buffer_item *
buffer_set_allocate_item(struct buffer_set *bs);

void
buffer_set_release_item(struct buffer_item *bi);

struct buffer_item *
buffer_set_allocate_item_from_pending(struct buffer_set *bs);

int
buffer_set_release_pending_alloc(struct buffer_set *bc, const size_t nitems);

int
buffer_set_pending_alloc(struct buffer_set *bs, const size_t nitems);

int
buffer_set_destroy(struct buffer_set *bs);

int
buffer_set_init(struct buffer_set *bs, void *region, size_t region_size,
                size_t nbufs, size_t buf_size, enum buffer_set_opts opts);

int
buffer_set_initx(struct buffer_set_args *bsa);

static inline ssize_t
buffer_user_list_total_bytes(const struct buffer_user_slist *bus,
                             size_t *nitems)
{
    if (!bus)
        return -EINVAL;

    if (*nitems)
        *nitems = 0;

    const struct buffer_item *bi = NULL;
    ssize_t total = 0;

    SLIST_FOREACH(bi, bus, bi_user_slentry)
    {
        total += bi->bi_iov_save.iov_len;
        (*nitems)++;
    }

    return total;
}

int
buffer_set_apply_name(struct buffer_set *bs, const char *name);

int
buffer_set_user_cache_release_item(
    struct buffer_item *bi, void (*revoke_cb)(struct buffer_item *, void *),
    void *arg);

unsigned int
buffer_get_alignment(enum buffer_set_opts opts);
#endif
