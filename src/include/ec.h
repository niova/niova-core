/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Paul Nowoczynski <00pauln00@gmail.com> 2018
 */
#ifndef EC_H
#define EC_H 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Upper bound on m = k + p defined in niova_block_common.h . */
#define NIOVA_EC_M_MAX   27

/* Per-instance encode cache.
 *
 * Holds the g_tbls for every k in [neec_k_min, neec_k_max] so the encode hot
 * path never regenerates a matrix.  An instance is owned by its caller (e.g. a
 * single vdev handle) which keeps encode tables local rather than process-wide.
 */
struct niova_ec_encode_cache
{
    uint8_t     *neec_gtbls[NIOVA_EC_M_MAX + 1];
    unsigned int neec_p;
    unsigned int neec_k_min;
    unsigned int neec_k_max;
    bool         neec_initialized;
};

/* Decode context.*/
struct niova_ec_decode
{
    unsigned int k;
    unsigned int nerrs;
    uint8_t      erased[NIOVA_EC_M_MAX];
    uint8_t      decode_index[NIOVA_EC_M_MAX];
    uint8_t      slot_of_frag[NIOVA_EC_M_MAX];
    uint8_t     *g_tbls;
};

/* Establishes the (k_min..k_max, p) geometry for a cache instance.
 *
 * Builds and caches g_tbls for every k in [k_min, k_max] so the encode hot
 * path never regenerates a matrix.  The caller owns `cache` and its lifetime.
 *
 */
int
niova_ec_init_encode_cache(struct niova_ec_encode_cache *cache,
                           unsigned int k_min, unsigned int k_max,
                           unsigned int p);

/* Release all cached tables */
void
niova_ec_destroy_encode_cache(struct niova_ec_encode_cache *cache);


/* One-shot encode: all k data fragments in memory, produces all p parity
 * fragments in one call. Use when the full stripe is assembled before ship.
 */
int
niova_ec_encode(const struct niova_ec_encode_cache *cache, unsigned int k,
                size_t len, uint8_t *const *data, uint8_t **parity);

/* Streaming encode: fold one data fragment into the running parity. Use when
 * fragments arrive over time and parity work should overlap with fragment
 * assembly. Caller zeros parity[] at stripe start, then calls once per
 * src_idx in [0, k) (any order, exactly once each).
 */
int
niova_ec_encode_update(const struct niova_ec_encode_cache *cache,
                       unsigned int k, unsigned int src_idx, size_t len,
                       const uint8_t *src, uint8_t **parity);

/* Build a decode context for a given erasure pattern.
 *
 * `erased_idx[0..nerrs-1]` lists missing fragment indices in [0, k+p).
 * Indices must be in range and unique, nerrs must be in [1, p] otherwise
 * data is not recoverable.
 *
 * After this call, `d->erased[i]` is the original frag idx that will be
 * written to rebuilt[i] by niova_ec_decode_update().
 */
int
niova_ec_decode_prepare(struct niova_ec_decode *d, unsigned int k,
                        unsigned int p, const unsigned int *erased_idx,
                        unsigned int nerrs);

/* Fold one surviving fragment into the running rebuild. The caller must:
 *  - zero rebuilt[0..d->nerrs-1] before the first call for a stripe (calloc
 *    works); update() XOR-accumulates into them.
 *  - input surviving fragments by their original index in [0, k+p). Any order.
 *
 * Returns 0 on success, -ENOENT if `frag_idx` is not part of the chosen
 * recovery set, -EINVAL on bad arguments.
 */
int
niova_ec_decode_update(const struct niova_ec_decode *d, unsigned int frag_idx,
                       size_t len, const uint8_t *src, uint8_t **rebuilt);

/* Release internal storage owned by `d`*/
void
niova_ec_decode_release(struct niova_ec_decode *d);

#endif //EC_H
