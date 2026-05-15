/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 */
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <isa-l.h>

#include "ec.h"
#include "log.h"

static uint8_t     *niovaEcGTbls[NIOVA_EC_M_MAX + 1];
static unsigned int niovaEcP;
static unsigned int niovaEcKMin;
static unsigned int niovaEcKMax;
static bool         niovaEcInitialized;

static int
niova_ec_build_slot(unsigned int k, unsigned int p)
{
    const unsigned int m = k + p;

    uint8_t *encode_matrix = calloc(1, (size_t)m * k);
    uint8_t *g_tbls        = calloc(1, 32UL * k * p);

    if (!encode_matrix || !g_tbls)
    {
        free(encode_matrix);
        free(g_tbls);
        return -ENOMEM;
    }

    gf_gen_cauchy1_matrix(encode_matrix, m, k);

    ec_init_tables(k, p, &encode_matrix[k * k], g_tbls);
    free(encode_matrix);

    niovaEcGTbls[k] = g_tbls;
    return 0;
}

int
niova_ec_init_encode_cache(unsigned int k_min, unsigned int k_max,
                           unsigned int p)
{
    if (niovaEcInitialized)
        return -EALREADY;

    if (p == 0 || k_min == 0 || k_min > k_max ||
        (k_max + p) > NIOVA_EC_M_MAX)
        return -EINVAL;

    for (unsigned int k = k_min; k <= k_max; k++)
    {
        int rc = niova_ec_build_slot(k, p);
        if (rc)
        {
            for (unsigned int j = k_min; j < k; j++)
            {
                free(niovaEcGTbls[j]);
                niovaEcGTbls[j] = NULL;
            }
            return rc;
        }
    }

    niovaEcP           = p;
    niovaEcKMin        = k_min;
    niovaEcKMax        = k_max;
    niovaEcInitialized = true;

    SIMPLE_LOG_MSG(LL_DEBUG,
                   "niova ec: cached encode tables for k=[%u..%u], p=%u",
                   k_min, k_max, p);
    return 0;
}

void
niova_ec_destroy_encode_cache(void)
{
    if (!niovaEcInitialized)
        return;

    for (unsigned int k = niovaEcKMin; k <= niovaEcKMax; k++)
    {
        free(niovaEcGTbls[k]);
    }
    niovaEcP           = 0;
    niovaEcKMin        = 0;
    niovaEcKMax        = 0;
    niovaEcInitialized = false;
}

int
niova_ec_encode(unsigned int k, size_t len, uint8_t *const *data,
                uint8_t **parity)
{
    if (!niovaEcInitialized || k < niovaEcKMin || k > niovaEcKMax ||
        !data || !parity || !len)
        return -EINVAL;

    /* ec_encode_data takes non-const data pointers but does not modify the
     * source buffers
     */
    ec_encode_data((int)len, (int)k, (int)niovaEcP, niovaEcGTbls[k],
                   (unsigned char **)(uintptr_t)data,
                   (unsigned char **)parity);

    return 0;
}

int
niova_ec_encode_update(unsigned int k, unsigned int src_idx, size_t len,
                       const uint8_t *src, uint8_t **parity)
{
    if (!niovaEcInitialized || k < niovaEcKMin || k > niovaEcKMax ||
        src_idx >= k || !src || !parity || !len)
        return -EINVAL;

    /* ec_encode_data_update takes non-const data pointers but does not modify
     * the source buffers
     */
    ec_encode_data_update((int)len, (int)k, (int)niovaEcP, (int)src_idx,
                          niovaEcGTbls[k],
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
                        const unsigned int *erased_idx, unsigned int nerrs)
{
    if (!niovaEcInitialized || !d || !erased_idx ||
        k < niovaEcKMin || k > niovaEcKMax ||
        nerrs == 0 || nerrs > niovaEcP)
        return -EINVAL;

    const unsigned int p = niovaEcP;
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

    uint8_t *g_tbls = calloc(1, 32UL * k * nerrs);
    if (!g_tbls)
        return -ENOMEM;

    int rc = niova_ec_gen_decode_matrix(encode_matrix, decode_matrix,
                                        invert_matrix, temp_matrix,
                                        d->decode_index, frag_err_list,
                                        nerrs, k, m);
    if (rc)
    {
        free(g_tbls);
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
    free(d->g_tbls);
    d->g_tbls = NULL;
    d->k      = 0;
    d->nerrs  = 0;
}
