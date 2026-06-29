/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 *
 * EC encode throughput microbenchmark.
 *
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "ec.h"
#include "log.h"

struct ec_geom
{
    unsigned int k;
    unsigned int p;
};

static const struct ec_geom geoms[] = {
    { 2, 1 }, { 4, 2 }, { 6, 2 }, { 8, 3 }, { 8, 4 },
    { 10, 4 }, { 12, 4 }, { 16, 4 }, { 16, 5 }, { 19, 8 },
};
static const size_t shards[] = { 32 * 1024, 64 * 1024, 128 * 1024 };

#define NELEMS(a) (sizeof(a) / sizeof((a)[0]))

/* Default user-byte budget B processed per (geom, shard) cell. */
#define DEFAULT_TAX_BYTES (1ULL << 30)

/* Per-update segment size: nbt issues one iovec per VBLK (4 KiB), so the
 * client folds parity 4 KiB at a time.  Match it.
 */
#define SEG_BYTES (4096)

static double
cpu_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* User-byte budget B; shared with the nbt sweep via EC_TAX_BYTES. */
static size_t
tax_bytes(void)
{
    const char *s = getenv("EC_TAX_BYTES");
    if (s)
    {
        char *end;
        unsigned long long v = strtoull(s, &end, 10);
        if (end != s && v)
            return (size_t)v;
    }
    return DEFAULT_TAX_BYTES;
}

static uint8_t **
alloc_shards(unsigned int n, size_t len)
{
    uint8_t **b = calloc(n, sizeof(*b));
    FATAL_IF(!b, "ec-micro-test: calloc failed");
    for (unsigned int i = 0; i < n; i++)
        FATAL_IF(posix_memalign((void **)&b[i], 64, len),
                 "ec-micro-test: posix_memalign(%zu) failed", len);
    return b;
}

static void
free_shards(uint8_t **b, unsigned int n)
{
    for (unsigned int i = 0; i < n; i++)
        free(b[i]);
    free(b);
}

/* Fold one full data shard into parity in VBLK-sized segments, exactly as
 * nclient_ec_fold_shard_into_parity() does when driven by nbt's 4 KiB iovecs.
 */
static void
fold_shard(const struct niova_ec_encode_cache *cache, unsigned int k,
           unsigned int src_idx, size_t shard, const uint8_t *src,
           uint8_t **parity, unsigned int p)
{
    uint8_t *par_at[NIOVA_EC_M_MAX];

    for (size_t off = 0; off < shard; off += SEG_BYTES)
    {
        const size_t seg = MIN((size_t)SEG_BYTES, shard - off);
        for (unsigned int j = 0; j < p; j++)
            par_at[j] = parity[j] + off;
        niova_ec_encode_update(cache, k, src_idx, seg, src + off, par_at);
    }
}

static void
run(const struct ec_geom *g, size_t shard, size_t bench_bytes)
{
    const unsigned int k = g->k;
    const unsigned int p = g->p;

    struct niova_ec_encode_cache cache = {0};
    int rc = niova_ec_init_encode_cache(&cache, k, k, p);
    FATAL_IF(rc, "init_encode_cache(%u,%u,%u) -> %d", k, k, p, rc);

    uint8_t **data   = alloc_shards(k, shard);
    uint8_t **parity = alloc_shards(p, shard);

    size_t niter = bench_bytes / ((size_t)k * shard);

    for (unsigned int w = 0; w < 8; w++)       /* warm tables + fault pages */
        for (unsigned int d = 0; d < k; d++)
            fold_shard(&cache, k, d, shard, data[d], parity, p);

    double t0 = cpu_sec();
    for (size_t it = 0; it < niter; it++)
        for (unsigned int d = 0; d < k; d++)
            fold_shard(&cache, k, d, shard, data[d], parity, p);
    double cpu = cpu_sec() - t0;

    double user_bytes = (double)k * shard * niter;
    double mibps      = user_bytes / (1024.0 * 1024.0) / (cpu > 0 ? cpu : 1e-9);
    double ns_per_b   = cpu * 1e9 / user_bytes;

    printf("%2u:%-2u  %9.1f  %9.4f  %9.0f  %8.3f\n",
           k, p, user_bytes / (1024.0 * 1024.0), cpu, mibps, ns_per_b);

    free_shards(parity, p);
    free_shards(data, k);
    niova_ec_destroy_encode_cache(&cache);
}

int
main(void)
{
    const size_t bench_bytes = tax_bytes();

    printf("ec-micro-test: niova_ec_encode_update throughput (B=%.0f MiB)\n",
           (double)bench_bytes / (1024.0 * 1024.0));

    for (size_t s = 0; s < NELEMS(shards); s++)
    {
        printf("\n== shard %zu KiB ==\n%-6s %9s  %9s  %9s  %8s\n",
               shards[s] / 1024, "geom", "data(MiB)", "cpu(s)", "MiB/s", "ns/B");
        for (size_t g = 0; g < NELEMS(geoms); g++)
            run(&geoms[g], shards[s], bench_bytes);
    }
    return 0;
}
