/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 *
 * Verifies that this machine's EC encode output matches the committed
 * reference (test/ec-reference-data.h), and that the streaming decoder
 * reconstructs lost fragments byte-identically to the originals.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ec.h"
#include "ec-reference-data.h"
#include "log.h"

/* Geometries exercised by both the reference generator and the test. Each
 * entry is a single (k, p, fragment_length) tuple. The set is small on
 * purpose so the committed reference header stays compact.
 */
struct ec_test_geom
{
    unsigned int k;
    unsigned int p;
    size_t       len;
    const char  *tag;
};

static const struct ec_test_geom ec_test_geoms[] = {
    { 4,  2, 4096, "k4_p2"  },
    { 8,  3, 8192, "k8_p3"  },
    { 16, 4, 512,  "k16_p4" },
};

#define EC_TEST_NGEOMS \
    (sizeof(ec_test_geoms) / sizeof(ec_test_geoms[0]))

/* Deterministic byte stream. The same (k, p, frag_idx) always produces the
 * same fragment contents on every machine, so the generator and the test
 * agree on the input. 64-bit LCG (Knuth's MMIX); top byte is folded in to
 * give every position a unique distribution.
 */
static inline void
ec_test_fill(uint8_t *buf, size_t len, unsigned int k, unsigned int p,
             unsigned int frag_idx)
{
    uint64_t s = 0x9E3779B97F4A7C15ULL
        ^ ((uint64_t)k << 32)
        ^ ((uint64_t)p << 24)
        ^ ((uint64_t)frag_idx << 8)
        ^ (uint64_t)len;

    for (size_t i = 0; i < len; i++)
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        buf[i] = (uint8_t)(s >> 56);
    }
}

static uint8_t ** alloc_buffers(unsigned int n, size_t len)
{
    uint8_t **b = calloc(n, sizeof(*b));
    FATAL_IF(!b, "ec-test: calloc(%u pointers) failed", n);

    for (unsigned int i = 0; i < n; i++)
    {
        b[i] = calloc(1, len);
        FATAL_IF(!b[i], "ec-test: calloc(%zu) failed", len);
    }
    return b;
}

static void free_buffers(uint8_t **b, unsigned int n)
{
    for (unsigned int i = 0; i < n; i++)
        free(b[i]);
    free(b);
}

static void
run_geom(const struct ec_test_geom *g, const struct ec_reference_entry *ref)
{
    FATAL_IF(ref->p != g->p,
             "geom %s: reference p=%u != geom p=%u", g->tag, ref->p, g->p);

    int rc = niova_ec_init_encode_cache(g->k, g->k, g->p);
    FATAL_IF(rc, "niova_ec_init_encode_cache(%u,%u,%u) -> %d",
             g->k, g->k, g->p, rc);

    const unsigned int m = g->k + g->p;
    uint8_t **data   = alloc_buffers(g->k, g->len);
    uint8_t **parity = alloc_buffers(g->p, g->len);

    // Fill with the same data the reference has been generated
    for (unsigned int i = 0; i < g->k; i++)
        ec_test_fill(data[i], g->len, g->k, g->p, i);

    //  Calculate parity
    rc = niova_ec_encode(g->k, g->len, data, parity);
    FATAL_IF(rc, "niova_ec_encode -> %d", rc);

    // Verify correctness against reference
    for (unsigned int i = 0; i < g->p; i++)
        FATAL_IF(memcmp(parity[i], ref->parity[i], g->len) != 0,
                 "geom %s: parity[%u] mismatch with reference", g->tag, i);

    // Save data
    uint8_t **shards = alloc_buffers(m, g->len);

    for (unsigned int i = 0; i < g->k; i++)
        memcpy(shards[i], data[i], g->len);
    for (unsigned int i = 0; i < g->p;i ++)
        memcpy(shards[g->k + i], parity[i], g->len);

    // Erase a mix: first half_data data shards, then half_par parity shards.
    const unsigned int nerrs     = g->p;
    const unsigned int half_data = nerrs / 2;
    const unsigned int half_par  = nerrs - half_data;
    unsigned int       erased[NIOVA_EC_M_MAX];
    bool               is_erased[NIOVA_EC_M_MAX] = {0};

    for (unsigned int i = 0; i < half_data; i++)
        erased[i] = i;
    for (unsigned int i = 0; i < half_par; i++)
        erased[half_data + i] = g->k + i;
    for (unsigned int i = 0; i < nerrs; i++)
        is_erased[erased[i]] = true;

    struct niova_ec_decode dctx = {0};
    rc = niova_ec_decode_prepare(&dctx, g->k, erased, nerrs);
    FATAL_IF(rc, "niova_ec_decode_prepare -> %d", rc);

    // calloc, so rebuilt[] starts zeroed as decode_update requires.
    uint8_t **rebuilt = alloc_buffers(nerrs, g->len);

    // Rebuild all missing blocks
    for (unsigned int idx = 0; idx < m; idx++)
    {
        if (is_erased[idx])
            continue;
        int s = niova_ec_decode_update(&dctx, idx, g->len, shards[idx],
                                       rebuilt);
        FATAL_IF(s && s != -ENOENT,
                 "niova_ec_decode_update(frag=%u) -> %d", idx, s);
    }

    for (unsigned int i = 0; i < nerrs; i++)
    {
        unsigned int frag = dctx.erased[i];
        const uint8_t *orig = (frag < g->k) ? data[frag]
                                            : parity[frag - g->k];
        FATAL_IF(memcmp(rebuilt[i], orig, g->len) != 0,
                 "geom %s: rebuilt[%u] (frag %u) mismatch",
                 g->tag, i, frag);
    }

    printf("ec-test %-7s k=%2u p=%u len=%5zu: encode matches reference, "
           "streaming decode rebuilt %u erased frags\n",
           g->tag, g->k, g->p, g->len, nerrs);

    niova_ec_decode_release(&dctx);
    free_buffers(rebuilt, nerrs);
    free_buffers(shards, m);
    free_buffers(parity, g->p);
    free_buffers(data, g->k);
    niova_ec_destroy_encode_cache();
}

int
main(void)
{
    for (size_t i = 0; i < EC_TEST_NGEOMS; i++)
        run_geom(&ec_test_geoms[i], &ec_reference_table[i]);
    return 0;
}

