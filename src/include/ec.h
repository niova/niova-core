/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Paul Nowoczynski <00pauln00@gmail.com> 2018
 */
#ifndef EC_H
#define EC_H 1

#include <stddef.h>
#include <stdint.h>

/* Upper bound on m = k + p defined in niova_block_common.h . */
#define NIOVA_EC_M_MAX   27

/* Cached encode tables for one geometry (k data shards, p parity shards). */
struct niova_ec_encode_cache
{
    uint8_t     *neec_gtbls;
    unsigned int neec_k;
    unsigned int neec_p;
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

/* Build tables for (k, p) in a zero-initialized, caller-owned cache. */
int
niova_ec_init_encode_cache(struct niova_ec_encode_cache *cache,
                           unsigned int k, unsigned int p);

/* Release all cached tables */
void
niova_ec_destroy_encode_cache(struct niova_ec_encode_cache *cache);

/* Process-wide pool of encode caches, keyed by (k, p) geometry.
 *
 * get()  builds one cache per distinct geometry the first time it is
 * requested and hands back the same pointer to every subsequent caller with
 * that geometry.
 *
 * Returns NULL on invalid geometry or allocation failure.
 */
const struct niova_ec_encode_cache *
niova_ec_encode_cache_pool_get(unsigned int k, unsigned int p);

/* One-shot encode: all k data fragments in memory, produces all p parity
 * fragments in one call. Use when the full stripe is assembled before ship.
 */
int
niova_ec_encode(const struct niova_ec_encode_cache *cache, size_t len,
                uint8_t *const *data, uint8_t **parity);

/* Streaming encode: fold one data fragment into the running parity. Use when
 * fragments arrive over time and parity work should overlap with fragment
 * assembly. Caller zeros parity[] at stripe start, then calls once per data
 * position src_idx in [0, k), in any order. Known-zero positions can be skipped.
 */
int
niova_ec_encode_update(const struct niova_ec_encode_cache *cache,
                       unsigned int src_idx, size_t len, const uint8_t *src,
                       uint8_t **parity);

/* Build a decode context for a given erasure pattern.
 *
 * `erased_idx[0..nerrs-1]` lists missing fragment indices in [0, k+p).
 * Indices must be in range and unique, nerrs must be in [1, p] otherwise
 * data is not recoverable.
 *
 * After this call, `d->erased[i]` is the original frag idx that will be
 * written to rebuilt[i] by niova_ec_decode_update().
 *
 * Note: This function is only used for testing.
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
 *
 * Note: This function is only used for testing.
 */
int
niova_ec_decode_update(const struct niova_ec_decode *d, unsigned int frag_idx,
                       size_t len, const uint8_t *src, uint8_t **rebuilt);

/* Release internal storage owned by `d`*/
void
niova_ec_decode_release(struct niova_ec_decode *d);

#endif //EC_H
