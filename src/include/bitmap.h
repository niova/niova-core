/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Paul Nowoczynski <pauln@niova.io> 2021
 */
#ifndef __NIOVA_BITMAP_H
#define __NIOVA_BITMAP_H 1

#include "common.h"
#include "log.h"

typedef uint64_t bitmap_word_t;
typedef uint32_t bitmap_word32_t;

#define NB_WORD_TYPE_SZ      (sizeof(bitmap_word_t))
#define NB_WORD_ANY          -1ULL
#define NB_MAX_IDX_ANY       -1U
#define NB_WORD_TYPE_SZ_BITS (NB_WORD_TYPE_SZ * NBBY)
#define NB_MAP_WORD_IDX(x)   ((x) / NB_WORD_TYPE_SZ_BITS)
#define NB_NUM_WORDS(nbits)  (NB_MAP_WORD_IDX(nbits) +                  \
                              ((nbits % NB_WORD_TYPE_SZ_BITS) ? 1 : 0))
#define NB_MAX_IDX_MASK      0x3f
#define NB_NUM_WORDS_MAX  \
    ((1ULL << (sizeof(unsigned int) * NBBY)) / NB_WORD_TYPE_SZ_BITS)

static inline void
bitmap_compile_time_asserts(void)
{
    COMPILE_TIME_ASSERT(NB_WORD_TYPE_SZ_BITS == (NB_MAX_IDX_MASK + 1));
}

struct niova_bitmap
{
    unsigned int   nb_nwords;
    unsigned int   nb_alloc_hint;
    unsigned int   nb_max_idx;
    bitmap_word_t *nb_map;
};

static inline unsigned int
niova_bitmap_max_word(const struct niova_bitmap *nb)
{
    return NB_NUM_WORDS(nb->nb_max_idx);
}

static inline int
niova_bitmap_attach(struct niova_bitmap *nb, bitmap_word_t *map,
                    unsigned int nwords)
{
    if (!nb || !map || !nwords)
        return -EINVAL;

    if (nwords >= NB_NUM_WORDS_MAX)
        return -E2BIG;

    nb->nb_max_idx = nwords * NB_WORD_TYPE_SZ_BITS;
    nb->nb_nwords = nwords;
    nb->nb_map = map;

    return 0;
}

static inline int
niova_bitmap_attach_max_idx(struct niova_bitmap *nb, bitmap_word_t *map,
                            unsigned int nwords, unsigned int max_idx)
{
    if (!nb || !map || !nwords)
        return -EINVAL;

    if (nwords >= NB_NUM_WORDS_MAX)
        return -E2BIG;

    if ((nwords * NB_WORD_TYPE_SZ_BITS) < max_idx)
        return -EOVERFLOW;

    nb->nb_max_idx = max_idx;
    nb->nb_nwords = nwords;
    nb->nb_map = map;

    return 0;
}

static inline int
niova_bitmap_set_unset(struct niova_bitmap *nb, unsigned int idx, bool set)
{
    if (!nb)
        return -EINVAL;

    if (idx >= nb->nb_max_idx)
        return -ERANGE;

    unsigned int word_idx = NB_MAP_WORD_IDX(idx);
    bitmap_word_t mask = ((bitmap_word_t)1) << (idx % NB_WORD_TYPE_SZ_BITS);

    if (set)
    {
        if (nb->nb_map[word_idx] & mask)
            return -EBUSY;

        nb->nb_map[word_idx] |= mask;
    }
    else
    {
        if (!(nb->nb_map[word_idx] & mask))
            return -EALREADY;

        nb->nb_map[word_idx] &= ~mask;
    }

    return 0;
}

static inline int
niova_bitmap_init(struct niova_bitmap *nb)
{
    if (!nb || !nb->nb_map || !nb->nb_nwords)
        return -EINVAL;

    if (nb->nb_nwords >= NB_NUM_WORDS_MAX)
        return -E2BIG;

    if (!nb->nb_max_idx)
        nb->nb_max_idx = (nb->nb_nwords * NB_WORD_TYPE_SZ_BITS);

    if (nb->nb_max_idx > (nb->nb_nwords * NB_WORD_TYPE_SZ_BITS))
        return -EOVERFLOW;

    memset(nb->nb_map, 0, nb->nb_nwords * NB_WORD_TYPE_SZ);

    // Bits beyond max_idx are logically disabled through last-word checks

    return 0;
}

static inline int
niova_bitmap_attach_and_init(struct niova_bitmap *nb, bitmap_word_t *map,
                             unsigned int nwords)
{
    int rc = niova_bitmap_attach(nb, map, nwords);

    return rc ? rc : niova_bitmap_init(nb);
}

static inline int
niova_bitmap_attach_and_init_max_idx(struct niova_bitmap *nb,
                                     bitmap_word_t *map, unsigned int nwords,
                                     unsigned int max_idx)
{
    int rc = niova_bitmap_attach_max_idx(nb, map, nwords, max_idx);

    return rc ? rc : niova_bitmap_init(nb);
}

static inline size_t
niova_bitmap_size_bits(const struct niova_bitmap *nb)
{
    return nb->nb_max_idx;
}

static inline size_t
niova_bitmap_inuse(const struct niova_bitmap *nb)
{
    if (!nb)
        return NB_MAX_IDX_ANY;

    size_t total = 0;
    unsigned int max_word = niova_bitmap_max_word(nb);

    for (unsigned int i = 0; i < max_word; i++)
        total += number_of_ones_in_val(nb->nb_map[i]);

    return total;
}

static inline size_t
niova_bitmap_nfree(const struct niova_bitmap *nb)
{
    return niova_bitmap_size_bits(nb) - niova_bitmap_inuse(nb);
}

static inline bool
niova_bitmap_full(const struct niova_bitmap *nb)
{
    return (niova_bitmap_inuse(nb) == nb->nb_max_idx);
}

static inline bool
niova_bitmap_is_set(const struct niova_bitmap *nb, unsigned int idx)
{
    if (!nb || idx >= nb->nb_max_idx)
        return false;

    unsigned int word_idx = NB_MAP_WORD_IDX(idx);
    bitmap_word_t mask = ((bitmap_word_t)1) << (idx % NB_WORD_TYPE_SZ_BITS);

    return (nb->nb_map[word_idx] & mask) ? true : false;
}

static inline int
niova_bitmap_unset(struct niova_bitmap *nb, unsigned int idx)
{
    if (idx >= nb->nb_max_idx)
        return -ERANGE;

    return niova_bitmap_set_unset(nb, idx, false);
}

static inline int
niova_bitmap_set(struct niova_bitmap *nb, unsigned int idx)
{
    if (nb->nb_max_idx && idx >= nb->nb_max_idx)
        return -ERANGE;

    return niova_bitmap_set_unset(nb, idx, true);
}

/* Bulk bit-range helpers: set/clear/test `width` contiguous bits starting at
 * `idx` in one word access instead of a bit-by-bit loop. `idx` must be a
 * multiple of `width`, and `width` must divide NB_WORD_TYPE_SZ_BITS (true
 * for e32/e64/e128 fvblks, which are 8/16/32 4k-vblks wide) - that keeps the
 * range inside a single word, no boundary-crossing to worry about.
 *
 * These are hot-path primitives (compaction/merge), so the one-word and
 * bounds contract is enforced via NIOVA_ASSERT rather than a return code -
 * every caller already guarantees it by construction (fat-vblk widths are
 * naturally aligned), so a violation here is a caller bug, not a runtime
 * condition to recover from. niova_bitmap_set()/unset() remain the
 * return-code-checked single-bit API for callers that need one.
 */
static inline void
niova_bitmap_range_validate_width(unsigned int idx, unsigned int width)
{
    NIOVA_ASSERT(width > 0 && width <= NB_WORD_TYPE_SZ_BITS);

    // NIOVA_ASSERT stringifies its condition into a printf format string,
    // so the modulo must be computed here rather than inline - a literal
    // '%' in the asserted expression becomes a bogus conversion specifier.
    unsigned int bit_off = idx % NB_WORD_TYPE_SZ_BITS;
    NIOVA_ASSERT(bit_off + width <= NB_WORD_TYPE_SZ_BITS);
}

static inline void
niova_bitmap_range_validate(const struct niova_bitmap *nb, unsigned int idx,
                            unsigned int width)
{
    NIOVA_ASSERT(nb && nb->nb_map);
    niova_bitmap_range_validate_width(idx, width);
    NIOVA_ASSERT((size_t)idx + width <= nb->nb_max_idx);
}

/* Callers below have already validated (idx, width) via
 * niova_bitmap_range_validate() before reaching this internal helper.
 */
static inline bitmap_word_t
niova_bitmap_range_mask(unsigned int idx, unsigned int width)
{
    unsigned int bit_off = idx % NB_WORD_TYPE_SZ_BITS;

    return width == NB_WORD_TYPE_SZ_BITS ?
        NB_WORD_ANY : ((((bitmap_word_t)1) << width) - 1) << bit_off;
}

/* True only if every bit in the range is set */
static inline bool
niova_bitmap_range_is_set(const struct niova_bitmap *nb, unsigned int idx,
                          unsigned int width)
{
    niova_bitmap_range_validate(nb, idx, width);

    bitmap_word_t mask = niova_bitmap_range_mask(idx, width);

    return (nb->nb_map[NB_MAP_WORD_IDX(idx)] & mask) == mask;
}

/* True if any bit in the range is set */
static inline bool
niova_bitmap_range_any_set(const struct niova_bitmap *nb, unsigned int idx,
                           unsigned int width)
{
    niova_bitmap_range_validate(nb, idx, width);

    bitmap_word_t mask = niova_bitmap_range_mask(idx, width);

    return (nb->nb_map[NB_MAP_WORD_IDX(idx)] & mask) != 0;
}

static inline int
niova_bitmap_range_set(struct niova_bitmap *nb, unsigned int idx,
                       unsigned int width, bool ebusy_ok)
{
    niova_bitmap_range_validate(nb, idx, width);

    if (!ebusy_ok && niova_bitmap_range_any_set(nb, idx, width))
        return -EBUSY;

    nb->nb_map[NB_MAP_WORD_IDX(idx)] |= niova_bitmap_range_mask(idx, width);
    return 0;
}

static inline int
niova_bitmap_range_unset(struct niova_bitmap *nb, unsigned int idx,
                         unsigned int width)
{
    niova_bitmap_range_validate(nb, idx, width);

    if (!niova_bitmap_range_is_set(nb, idx, width))
        return -EALREADY;

    nb->nb_map[NB_MAP_WORD_IDX(idx)] &= ~niova_bitmap_range_mask(idx, width);
    return 0;
}

/* 0 == fully free, width == fully set, else == partially set */
static inline unsigned int
niova_bitmap_range_popcount(const struct niova_bitmap *nb, unsigned int idx,
                            unsigned int width)
{
    niova_bitmap_range_validate(nb, idx, width);

    return number_of_ones_in_val(
        nb->nb_map[NB_MAP_WORD_IDX(idx)] & niova_bitmap_range_mask(idx, width));
}

static inline int
niova_bitmap_copy(struct niova_bitmap *dest,
                  const struct niova_bitmap *src)
{
    if (!dest || !src || !src->nb_map || !dest->nb_map ||
        dest->nb_nwords != src->nb_nwords)
        return -EINVAL;

    for (unsigned int i = 0; i < dest->nb_nwords; i++)
        dest->nb_map[i] = src->nb_map[i];

    return 0;

}

static inline int
niova_bitmap_exclusive(const struct niova_bitmap *x,
                       const struct niova_bitmap *y)
{
    if (!x || !y || x->nb_nwords != y->nb_nwords)
        return -EINVAL;

    for (unsigned int i = 0; i < x->nb_nwords; i++)
    {
        // Ensure the bits from the src map are not already set in the dst
        if (((x->nb_map[i] ^ y->nb_map[i]) & y->nb_map[i]) != y->nb_map[i])
            return -EALREADY;
    }

    return 0;
}

static inline int
niova_bitmap_shared(const struct niova_bitmap *super,
                    const struct niova_bitmap *sub)
{
    if (!super || !sub || super->nb_nwords != sub->nb_nwords)
        return -EINVAL;

    for (unsigned int i = 0; i < sub->nb_nwords; i++)
    {
        // Ensure the bits from the src map are set in the dst
        if ((super->nb_map[i] & sub->nb_map[i]) != sub->nb_map[i])
            return -ENOENT;
    }

    return 0;
}

static inline int
niova_bitmap_merge(struct niova_bitmap *dst, const struct niova_bitmap *src)
{
    int rc = niova_bitmap_exclusive(dst, src);

    if (!rc)
    {
        for (unsigned int i = 0; i < dst->nb_nwords; i++)
            dst->nb_map[i] |= src->nb_map[i];
    }

    return rc;
}

// Unset the items in 'dst' which are contained in 'src'
static inline int
niova_bitmap_bulk_unset(struct niova_bitmap *dst,
                        const struct niova_bitmap *src)
{
    int rc = niova_bitmap_shared(dst, src);

    if (!rc)
    {
        for (unsigned int i = 0; i < dst->nb_nwords; i++)
            dst->nb_map[i] &= ~(src->nb_map[i]);
    }

    return rc;
}

static inline int
niova_bitmap_lowest_free_bit_assign(struct niova_bitmap *nb, unsigned int *idx)
{
    if (!nb || !idx)
        return -EINVAL;

    unsigned int nw = niova_bitmap_max_word(nb);

    const unsigned int start_idx =
        nw > nb->nb_alloc_hint ? nb->nb_alloc_hint : 0;

    for (unsigned int i = 0; i < nw; i++)
    {
        unsigned int sii = (start_idx + i) % nw;
        unsigned int lw_bits = 0;

        if ((sii == (nw - 1)) && (nb->nb_max_idx & NB_MAX_IDX_MASK))
        {
            lw_bits =
                NB_WORD_TYPE_SZ_BITS - (nb->nb_max_idx & NB_MAX_IDX_MASK);

            nb->nb_map[sii] |= (-1ULL << (NB_WORD_TYPE_SZ_BITS - lw_bits));
        }

        if (nb->nb_map[sii] != NB_WORD_ANY)
        {
            uint64_t x = lowest_bit_set_and_return(&nb->nb_map[sii]);

            *idx = ((sii * NB_WORD_TYPE_SZ_BITS) +
                    (highest_set_bit_pos_from_val(x) - 1));

            NIOVA_ASSERT(*idx < nb->nb_max_idx);

            nb->nb_alloc_hint = sii;

            if (lw_bits)
                nb->nb_map[sii] &= ~(-1ULL << (NB_WORD_TYPE_SZ_BITS - lw_bits));

            return 0;
        }

        if (lw_bits)
            nb->nb_map[sii] &= ~(-1ULL << (NB_WORD_TYPE_SZ_BITS - lw_bits));
    }
    return -ENOSPC;
}

static inline int
niova_bitmap_lowest_used_bit_release(struct niova_bitmap *nb,
                                     unsigned int *idx)
{
    if (!nb || !idx)
        return -EINVAL;

    unsigned int nw = nb->nb_nwords;

    for (unsigned int i = 0; i < nw; i++)
    {
        if (nb->nb_map[i] != 0)
        {
            uint64_t x = lowest_bit_unset_and_return(&nb->nb_map[i]);

            *idx = ((i * NB_WORD_TYPE_SZ_BITS) +
                    (highest_set_bit_pos_from_val(x) - 1));

            return 0;
        }
    }
    return -ENOENT;
}

static inline int
niova_bitmap_get_32bit_word(const struct niova_bitmap *nb, unsigned int idx32,
                            unsigned int *ret_u32)
{
    if (!nb || !ret_u32)
        return -EINVAL;

    if (idx32 >= (nb->nb_nwords * 2))
        return -ERANGE;

    const uint32_t *map_u32 = (uint32_t *)nb->nb_map;

    *ret_u32 = map_u32[idx32];

    return 0;
}

static inline void
niova_bitmap_compile_time_asserts(void)
{
    COMPILE_TIME_ASSERT(NB_WORD_TYPE_SZ_BITS == 64);
}

static inline int
niova_bitmap_iterate(const struct niova_bitmap *nb,
                     void (*cb)(size_t idx, size_t len, void *), void *arg)
{
    if (nb == NULL || cb == NULL)
        return -EINVAL;

    size_t max_idx = niova_bitmap_size_bits(nb);
    if (max_idx == NB_MAX_IDX_ANY)
        return -EINVAL;

    ssize_t idx = -1;
    size_t len = 0;

    for (size_t i = 0; i < max_idx; i++)
    {
        if (niova_bitmap_is_set(nb, i))
        {
            if (idx < 0)
            {
                idx = i;
                len = 1;
            }
            else
            {
                len++;
            }
        }
        else
        {
            if (idx >= 0)
            {
                cb(idx, len, arg);
                idx = -1;
            }
        }
    }

    if (idx >= 0)
        cb(idx, len, arg);

    return 0;
}

#endif
