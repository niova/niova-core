/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 */
#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <isa-l.h>

#include "alloc.h"
#include "ctor.h"
#include "ec.h"
#include "log.h"

REGISTRY_ENTRY_FILE_GENERATE;

static int
niova_ec_build_slot(struct niova_ec_encode_cache *cache, unsigned int k,
                    unsigned int p)
{
    const unsigned int m = k + p;

    uint8_t *encode_matrix = niova_calloc_can_fail(1, (size_t)m * k);
    uint8_t *g_tbls        =
        niova_posix_memalign(32UL * k * p, L2_CACHELINE_SIZE_BYTES);

    if (!encode_matrix || !g_tbls)
    {
        niova_free(encode_matrix);
        niova_free(g_tbls);
        return -ENOMEM;
    }

    gf_gen_cauchy1_matrix(encode_matrix, m, k);

    ec_init_tables(k, p, &encode_matrix[k * k], g_tbls);
    niova_free(encode_matrix);

    cache->neec_gtbls[k] = g_tbls;
    return 0;
}

int
niova_ec_init_encode_cache(struct niova_ec_encode_cache *cache,
                           unsigned int k_min, unsigned int k_max,
                           unsigned int p)
{
    if (!cache)
        return -EINVAL;

    if (cache->neec_initialized)
        return (k_min == cache->neec_k_min && k_max == cache->neec_k_max &&
                p == cache->neec_p) ? -EALREADY : -EINVAL;

    if (p == 0 || k_min == 0 || k_min > k_max ||
        (k_max + p) > NIOVA_EC_M_MAX)
        return -EINVAL;

    for (unsigned int k = k_min; k <= k_max; k++)
    {
        int rc = niova_ec_build_slot(cache, k, p);
        if (rc)
        {
            for (unsigned int j = k_min; j < k; j++)
            {
                niova_free(cache->neec_gtbls[j]);
                cache->neec_gtbls[j] = NULL;
            }
            return rc;
        }
    }

    cache->neec_p           = p;
    cache->neec_k_min       = k_min;
    cache->neec_k_max       = k_max;
    cache->neec_initialized = true;

    SIMPLE_LOG_MSG(LL_DEBUG,
                   "niova ec: cached encode tables for k=[%u..%u], p=%u",
                   k_min, k_max, p);
    return 0;
}

void
niova_ec_destroy_encode_cache(struct niova_ec_encode_cache *cache)
{
    if (!cache || !cache->neec_initialized)
        return;

    for (unsigned int k = cache->neec_k_min; k <= cache->neec_k_max; k++)
    {
        niova_free(cache->neec_gtbls[k]);
        cache->neec_gtbls[k] = NULL;
    }
    cache->neec_p           = 0;
    cache->neec_k_min       = 0;
    cache->neec_k_max       = 0;
    cache->neec_initialized = false;
}

/* Pool entries are indexed [k_max][p]. k_min isn't part of the index since,
 * for every current caller, it's a pure function of (k_max, p).
 */
static struct niova_ec_encode_cache *
niovaEcEncodeCachePool[NIOVA_EC_M_MAX + 1][NIOVA_EC_M_MAX + 1];

const struct niova_ec_encode_cache *
niova_ec_encode_cache_pool_get(unsigned int k_min, unsigned int k_max,
                               unsigned int p)
{
    if (p == 0 || k_min == 0 || k_min > k_max || (k_max + p) > NIOVA_EC_M_MAX)
        return NULL;

    struct niova_ec_encode_cache *cache = niovaEcEncodeCachePool[k_max][p];

    if (cache)
    {
        NIOVA_ASSERT(cache->neec_k_min == k_min);
    }
    else
    {
        cache = niova_calloc_can_fail(1, sizeof(struct niova_ec_encode_cache));
        if (cache && niova_ec_init_encode_cache(cache, k_min, k_max, p))
        {
            niova_free(cache);
            cache = NULL;
        }

        niovaEcEncodeCachePool[k_max][p] = cache;
    }

    return cache;
}

static destroy_ctx_t NIOVA_DESTRUCTOR(EC_ENCODE_CACHE_POOL_CTOR_PRIORITY)
niova_ec_encode_cache_pool_destroy(void)
{
    for (unsigned int k = 0; k <= NIOVA_EC_M_MAX; k++)
    {
        for (unsigned int p = 0; p <= NIOVA_EC_M_MAX; p++)
        {
            struct niova_ec_encode_cache *cache = niovaEcEncodeCachePool[k][p];
            if (cache)
            {
                niova_ec_destroy_encode_cache(cache);
                niova_free(cache);
                niovaEcEncodeCachePool[k][p] = NULL;
            }
        }
    }
}

int
niova_ec_encode(const struct niova_ec_encode_cache *cache, unsigned int k,
                size_t len, uint8_t *const *data, uint8_t **parity)
{
    if (!cache || !cache->neec_initialized ||
        k < cache->neec_k_min || k > cache->neec_k_max ||
        !data || !parity || !len)
        return -EINVAL;

    /* ec_encode_data takes non-const data pointers but does not modify the
     * source buffers
     */
    ec_encode_data((int)len, (int)k, (int)cache->neec_p, cache->neec_gtbls[k],
                   (unsigned char **)(uintptr_t)data,
                   (unsigned char **)parity);

    return 0;
}

int
niova_ec_encode_update(const struct niova_ec_encode_cache *cache,
                       unsigned int k, unsigned int src_idx, size_t len,
                       const uint8_t *src, uint8_t **parity)
{
    if (!cache || !cache->neec_initialized ||
        k < cache->neec_k_min || k > cache->neec_k_max ||
        src_idx >= k || !src || !parity || !len)
        return -EINVAL;

    /* ec_encode_data_update takes non-const data pointers but does not modify
     * the source buffers
     */
    ec_encode_data_update((int)len, (int)k, (int)cache->neec_p, (int)src_idx,
                          cache->neec_gtbls[k],
                          (unsigned char *)(uintptr_t)src,
                          (unsigned char **)parity);

    return 0;
}

/* Builds the decode matrix from the encode matrix + erasure list, using the
 * pattern from isa-l's ec_simple_example.c. Plain-English summary:
 *  1) drop the rows of encode_matrix that correspond to erased fragments,
 *  2) invert the resulting kxk submatrix (Cauchy guarantees this works),
 *  3) the inverse's rows are the recipe to rebuild each original data shard;
 *     for an erased parity shard, multiply its original encode row by the
 *     inverse to re-express it in terms of the surviving shards.
 *
 * Also fills decode_index[] with the surviving frag indices in matrix order.
 */
static int
niova_ec_gen_decode_matrix(const uint8_t *encode_matrix, uint8_t *decode_matrix,
                           uint8_t *invert_matrix, uint8_t *temp_matrix,
                           uint8_t *decode_index, const uint8_t *frag_err_list,
                           unsigned int nerrs, unsigned int k, unsigned int m)
{
    uint8_t frag_in_err[NIOVA_EC_M_MAX] = {0};
    uint8_t *b = temp_matrix;

    for (unsigned int i = 0; i < nerrs; i++)
        frag_in_err[frag_err_list[i]] = 1;

    for (unsigned int i = 0, r = 0; i < k; i++, r++)
    {
        while (r < m && frag_in_err[r])
            r++;
        if (r >= m)
            return -EINVAL;
        for (unsigned int j = 0; j < k; j++)
            b[k * i + j] = encode_matrix[k * r + j];
        decode_index[i] = (uint8_t)r;
    }

    if (gf_invert_matrix(b, invert_matrix, (int)k) < 0)
        return -EINVAL;

    for (unsigned int i = 0; i < nerrs; i++)
    {
        if (frag_err_list[i] < k)
        {
            for (unsigned int j = 0; j < k; j++)
                decode_matrix[k * i + j] =
                    invert_matrix[k * frag_err_list[i] + j];
        }
        else
        {
            for (unsigned int j = 0; j < k; j++)
            {
                uint8_t s = 0;
                for (unsigned int n = 0; n < k; n++)
                    s ^= gf_mul(invert_matrix[n * k + j],
                                encode_matrix[k * frag_err_list[i] + n]);
                decode_matrix[k * i + j] = s;
            }
        }
    }
    return 0;
}

int
niova_ec_decode_prepare(struct niova_ec_decode *d, unsigned int k,
                        unsigned int p, const unsigned int *erased_idx,
                        unsigned int nerrs)
{
    if (!d || !erased_idx || k == 0 || p == 0 ||
        (k + p) > NIOVA_EC_M_MAX || nerrs == 0 || nerrs > p)
        return -EINVAL;

    const unsigned int m = k + p;

    uint8_t seen[NIOVA_EC_M_MAX] = {0};
    uint8_t frag_err_list[NIOVA_EC_M_MAX];
    for (unsigned int i = 0; i < nerrs; i++)
    {
        if (erased_idx[i] >= m || seen[erased_idx[i]])
            return -EINVAL;
        seen[erased_idx[i]] = 1;
        frag_err_list[i] = (uint8_t)erased_idx[i];
    }

    uint8_t encode_matrix[NIOVA_EC_M_MAX * NIOVA_EC_M_MAX];
    uint8_t decode_matrix[NIOVA_EC_M_MAX * NIOVA_EC_M_MAX];
    uint8_t invert_matrix[NIOVA_EC_M_MAX * NIOVA_EC_M_MAX];
    uint8_t temp_matrix[NIOVA_EC_M_MAX * NIOVA_EC_M_MAX];

    gf_gen_cauchy1_matrix(encode_matrix, (int)m, (int)k);

    uint8_t *g_tbls = niova_posix_memalign(32UL * k * nerrs, L2_CACHELINE_SIZE_BYTES);
    if (!g_tbls)
        return -ENOMEM;

    int rc = niova_ec_gen_decode_matrix(encode_matrix, decode_matrix,
                                        invert_matrix, temp_matrix,
                                        d->decode_index, frag_err_list,
                                        nerrs, k, m);
    if (rc)
    {
        niova_free(g_tbls);
        return rc;
    }

    ec_init_tables((int)k, (int)nerrs, decode_matrix, g_tbls);

    memset(d->slot_of_frag, 0xff, sizeof(d->slot_of_frag));
    for (unsigned int i = 0; i < k; i++)
        d->slot_of_frag[d->decode_index[i]] = (uint8_t)i;

    for (unsigned int i = 0; i < nerrs; i++)
        d->erased[i] = frag_err_list[i];

    d->k       = k;
    d->nerrs   = nerrs;
    d->g_tbls  = g_tbls;
    return 0;
}

int
niova_ec_decode_update(const struct niova_ec_decode *d, unsigned int frag_idx,
                       size_t len, const uint8_t *src, uint8_t **rebuilt)
{
    if (!d || !src || !rebuilt || !len || frag_idx >= NIOVA_EC_M_MAX)
        return -EINVAL;

    uint8_t slot = d->slot_of_frag[frag_idx];
    if (slot >= d->k)
        return -ENOENT;

    /* ec_encode_data_update takes non-const data pointers but does not modify
     * the source buffer.
     */
    ec_encode_data_update((int)len, (int)d->k, (int)d->nerrs, (int)slot,
                          d->g_tbls,
                          (unsigned char *)(uintptr_t)src,
                          (unsigned char **)rebuilt);
    return 0;
}

void
niova_ec_decode_release(struct niova_ec_decode *d)
{
    if (!d)
        return;
    niova_free(d->g_tbls);
    d->g_tbls = NULL;
    d->k      = 0;
    d->nerrs  = 0;
}
