/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Paul Nowoczynski <pauln@niova.io> 2019
 */
#ifndef NIOVA_ALLOC_H
#define NIOVA_ALLOC_H 1

#ifndef __USE_GNU // reallocarray
#define __USE_GNU
#endif
#include <stdlib.h>

#include "common.h"
#include "log.h"

extern enum log_level allocLogLevel;

#define niova_malloc(size)                                     \
({                                                             \
    void *ptr = malloc(size);                                  \
    FATAL_IF_strerror((!ptr), "niova_malloc: ");               \
    LOG_MSG(allocLogLevel, "niova_malloc: %p %zu", ptr, size); \
    ptr;                                                       \
})

#define niova_malloc_can_fail(size)                            \
({                                                             \
    void *ptr = malloc(size);                                  \
    LOG_MSG(allocLogLevel, "niova_malloc: %p %zu", ptr, size); \
    ptr;                                                       \
})

#define niova_calloc(nmemb, size)                                         \
({                                                                        \
    void *ptr = calloc(nmemb, size);                                      \
    FATAL_IF_strerror((!ptr), "niova_calloc: ");                          \
    LOG_MSG(allocLogLevel, "niova_calloc: %p %zu %zu", ptr, nmemb, size); \
    ptr;                                                                  \
})

#define niova_calloc_can_fail(nmemb, size)                                \
({                                                                        \
    void *ptr = calloc(nmemb, size);                                      \
    LOG_MSG(allocLogLevel, "niova_calloc: %p %zu %zu", ptr, nmemb, size); \
    ptr;                                                                  \
})

#define niova_posix_memalign(size, alignment)                      \
({                                                                 \
    void *ptr = NULL;                                              \
    int rc = posix_memalign(&ptr, alignment, size);                \
    if (rc) ptr = NULL;                                            \
    LOG_MSG((rc ? LL_ERROR : allocLogLevel),                       \
            "niova_posix_memalign: %p %zu %zu: %s",                \
            ptr, size, alignment, rc ? strerror(rc) : "OK");       \
    ptr;                                                           \
})

#define niova_free(ptr)                            \
{                                                  \
    LOG_MSG(allocLogLevel, "niova_free: %p", ptr); \
    free(ptr);                                     \
}

#define niova_reallocarray(ptr, type, nmemb)                           \
({                                                                     \
    type *tmp = reallocarray((ptr), nmemb, sizeof(type));              \
                                                                       \
    LOG_MSG(allocLogLevel, "niova_reallocarray: src=%p dst=%p sz=%zu", \
            ptr, tmp, (size_t)(sizeof(type) * nmemb));                 \
                                                                       \
    if (tmp)                                                           \
        (ptr) = tmp;                                                   \
                                                                       \
    tmp ? 0 : -ENOMEM;                                                 \
})

void
alloc_log_level_set(enum log_level ll);

struct niova_env_var;

void
alloc_env_var_cb(const struct niova_env_var *nev);

#define NIOVA_VBA_MAX_BITS (NBBY * sizeof(uint64_t))
#define NIOVA_VBA_REGSZ_BITS 57
#define NIOVA_VBA_NUNITS_BITS 7
#define NIOVA_VBA_NUNITS_MASK ((1ULL << NIOVA_VBA_NUNITS_BITS) - 1)
#define NIOVA_VBA_REGSZ_MASK ((1ULL << NIOVA_VBA_REGSZ_BITS) - 1)

/**
 * niova_vbasic_allocator - structure for managing a "very basic allocation"
 *    scheme.  This is typically used for providing a simple allocator for
 *    statically allocated heaps.  Allocations may consist multiple units so
 *    long as contiguous ranges are available.
 * @nvba_alignment: optional alignment parameter
 * @nvba_unit_size: size of the allocation unit
 * @nvba_nunits: the number of allocatable units (used for testing)
 * @nvba_region_sz: the effective region size used by the vba (for testing)
 * @nvba_bitmap: allocation bitmap
 * @nvba_region_ptr: holds address of the usable region which may have been
 *    adjusted to meet the reqested alignment
 * @nvba_region:  the underlying buffer area
 */
struct niova_vbasic_allocator
{
    unsigned int nvba_alignment;
    uint32_t     nvba_unit_size; // size represented by each bit
    uint64_t     nvba_nunits:NIOVA_VBA_NUNITS_BITS;
    uint64_t     nvba_region_sz:NIOVA_VBA_REGSZ_BITS;
    uint64_t     nvba_bitmap;
    char        *nvba_region_ptr;
    char         nvba_region[];
};

static inline size_t
niova_vbasic_nassigned(const struct niova_vbasic_allocator *nvba)
{
    if (!nvba || nvba->nvba_bitmap == -1ULL)
        return NIOVA_VBA_MAX_BITS;

    else if (nvba->nvba_bitmap == 0)
        return 0;

    return number_of_ones_in_val(nvba->nvba_bitmap);
}

static inline int
niova_vbasic_init(struct niova_vbasic_allocator *nvba, size_t region_size)
{
    if (!nvba)
        return -EINVAL;

    unsigned int unit_size = region_size / NIOVA_VBA_MAX_BITS;

    if (unit_size == 0 || unit_size > UINT32_MAX)
        return -EINVAL;

    CONST_OVERRIDE(uint32_t, nvba->nvba_unit_size, unit_size);
    nvba->nvba_bitmap = 0;
    nvba->nvba_alignment = 0;
    nvba->nvba_region_sz = region_size;

    nvba->nvba_region_ptr = &nvba->nvba_region[0];
    return 0;
}

static inline int
niova_vbasic_init_aligned(struct niova_vbasic_allocator *nvba,
                          size_t region_size, size_t unit_size,
                          uint32_t alignment)
{
    int rc = 0, err_loc = 0;
    unsigned int nunits = 0;
    enum log_level log_lvl;
    // Basic parameter checks
    if (!nvba || region_size == 0 || alignment == 0)
    {
        err_loc = 0;
        rc = -EINVAL;
        goto xerror;
    }

    if (region_size > NIOVA_VBA_REGSZ_MASK)
    {
        err_loc = 1;
        rc = -E2BIG;
        goto xerror;
    }

    // Check that alignment is power of 2
    if (number_of_ones_in_val32(alignment) != 1)
    {
        err_loc = 2;
        rc = -EDOM;
        goto xerror;
    }

    const uintptr_t raw = (uintptr_t)&nvba->nvba_region[0];
    const uintptr_t aligned = (uintptr_t)ALIGNUP_PTR(raw, alignment);
    const size_t shifted = (size_t)(aligned - raw);

    if (shifted >= region_size)
    {
        err_loc = 3;
        rc = -ENOSPC;
        goto xerror;
    }

    /* Reduce region size per the requested alignment */
    region_size -= shifted;

    if (unit_size == 0)
    {
        unit_size = region_size / NIOVA_VBA_MAX_BITS;

        /* Align down unit size */
        unit_size = unit_size & ~(alignment - 1);
    }

    if (unit_size == 0 || unit_size > UINT32_MAX)
    {
        err_loc = 4;
        rc = -EINVAL;
        goto xerror;
    }

    if (unit_size < (size_t)alignment)
    {
        err_loc = 5;
        rc = -EOVERFLOW;
        goto xerror;
    }

    /* Unit size is multiple of alignment */
    if (unit_size & (alignment - 1))
    {
        err_loc = 6;
        rc = -EDOM;
        goto xerror;
    }

    nunits = region_size / unit_size;

    if (nunits == 0)
    {
        err_loc = 7;
        rc = -EINVAL;
        goto xerror;
    }

    // Start initialization of vba
    CONST_OVERRIDE(uint32_t, nvba->nvba_unit_size, unit_size);
    nvba->nvba_bitmap = 0;
    nvba->nvba_alignment = alignment;

    /* Max cap is this many num of units */
    if (nunits > NIOVA_VBA_MAX_BITS)
    {
        nunits = NIOVA_VBA_MAX_BITS;

        /* Shrink region size accordingly */
        region_size = nunits * unit_size;
    }
    else if (nunits < NIOVA_VBA_MAX_BITS)
    {
        /* Support < NIOVA_VBA_MAX_BITS num of units. Make rest of the
         * bits in the bitmap 1 so that those bits are unavailable.
         */
        uint64_t bitmap_mask = (nunits == 0) ? 0ULL : ((1ULL << nunits) - 1);
        nvba->nvba_bitmap = ~bitmap_mask;
    }

    nvba->nvba_region_sz = region_size;
    nvba->nvba_nunits = nunits;

    NIOVA_ASSERT(IS_ALIGNED_PTR(aligned, alignment));
    nvba->nvba_region_ptr = (char *)aligned;

xerror:
    log_lvl = rc ? LL_ERROR : LL_NOTIFY;
    SIMPLE_LOG_MSG(log_lvl, "reg_sz %ld nvba %p bmap 0x%lx usize %ld "
                   "nunits=%u align %u err_loc %d error %d",
                   region_size, nvba, nvba ? nvba->nvba_bitmap : 0,
                   unit_size, nunits, alignment, err_loc, rc);
    return rc;
}

static inline int
niova_vbasic_space_avail_nitems(const struct niova_vbasic_allocator *nvba,
                                size_t size_in_bytes, size_t nitems)
{
    if (!nvba || !size_in_bytes || !nitems)
        return -EINVAL;

    const unsigned int nunits =
        (size_in_bytes / nvba->nvba_unit_size) +
         (size_in_bytes % nvba->nvba_unit_size ? 1 : 0);

    if (nunits * nitems > NIOVA_VBA_MAX_BITS)
        return -E2BIG;

    int rc = 0;
    unsigned int offset = 0;
    for (size_t i = 0; i < nitems; i++)
    {
        rc = nconsective_bits_avail_offset(&nvba->nvba_bitmap, nunits, offset);
        if (rc < 0)
            return rc;

        offset = rc + nunits;
    }

    return 0; // return '0' on successful_ping_until_viable
}

static inline int
niova_vbasic_space_avail(const struct niova_vbasic_allocator *nvba,
                         size_t size_in_bytes)
{
    return niova_vbasic_space_avail_nitems(nvba, size_in_bytes, 1);
}

static inline int
niova_vbasic_malloc(struct niova_vbasic_allocator *nvba, size_t size_in_bytes,
                    void **ret_ptr)
{
    if (!nvba || !size_in_bytes || !ret_ptr)
        return -EINVAL;

    const unsigned int nunits =
        (size_in_bytes / nvba->nvba_unit_size +
         (size_in_bytes % nvba->nvba_unit_size ? 1 : 0));

    if (nunits > NIOVA_VBA_MAX_BITS)
        return -E2BIG;

    int offset = nconsective_bits_assign(&nvba->nvba_bitmap, nunits);
    if (offset < 0)
        return offset;

    char *ptr = &nvba->nvba_region_ptr[offset * nvba->nvba_unit_size];
    NIOVA_ASSERT(!nvba->nvba_alignment ||
                 ((uintptr_t)ptr & (nvba->nvba_alignment - 1)) == 0);

    *ret_ptr = ptr;

    return 0;
}

static inline int
niova_vbasic_free(struct niova_vbasic_allocator *nvba, const void *ptr,
                  size_t size_in_bytes)
{
    if (!nvba || !ptr || !size_in_bytes)
        return -EINVAL;

    uintptr_t my_ptr = (uintptr_t)ptr;
    uintptr_t region_base = (uintptr_t)nvba->nvba_region_ptr;
    uintptr_t region_end =
        (uintptr_t)((char *)region_base + nvba->nvba_region_sz);

    if (my_ptr < region_base || my_ptr >= region_end)
        return -ERANGE; // ptr value not within the allocation region

    const uintptr_t ptr_diff =
        (uintptr_t)((char *)my_ptr - nvba->nvba_region_ptr);

    if (ptr_diff % nvba->nvba_unit_size)
        return -EFAULT; // ptr is not aligned with the unit size

    unsigned int offset = ptr_diff / nvba->nvba_unit_size;

    const unsigned int nunits =
        (size_in_bytes / nvba->nvba_unit_size +
         (size_in_bytes % nvba->nvba_unit_size ? 1 : 0));

    return nconsective_bits_release(&nvba->nvba_bitmap, offset, nunits);
}

static inline uintptr_t
niova_vbasic_get_start_addr(struct niova_vbasic_allocator *nvba)
{
    return nvba ? (uintptr_t)nvba->nvba_region_ptr : (uintptr_t)NULL;
}

#endif
