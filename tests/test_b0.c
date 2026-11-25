#include "cache.h"

#include <assert.h>
#include <stdio.h>

typedef struct { size_t calls; int forbid; } source_t;

static int load(void *context, uint64_t id, void *destination, size_t size)
{
    source_t *source = context;
    source->calls++;
    if (source->forbid)
        return -1;
    unsigned char *bytes = destination;
    for (size_t i = 0; i < size; i++)
        bytes[i] = (unsigned char)((id * 17 + i) % 251);
    return 0;
}

static void assert_zero(cache_t *cache)
{
    cache_stats_t stats = cache_get_stats(cache);
    assert(stats.requests == 0 && stats.hits == 0 && stats.misses == 0 &&
           stats.evictions == 0 && stats.load_errors == 0);
}

static void test_warm_then_hits(void)
{
    source_t source = {0};
    unsigned char bytes[8];
    cache_t *cache = cache_create(4, 16, load, &source);
    assert(cache != NULL && cache_enable_usage(cache) == 0);
    for (uint64_t id = 0; id < 4; id++)
        assert(cache_read(cache, id, 0, bytes, 8) == 0);
    assert(source.calls == 4 && cache_get_stats(cache).misses == 4);
    assert(cache_reset_observations(cache) == 0);
    assert_zero(cache);
    cache_usage_stats_t usage = cache_get_usage(cache);
    assert(usage.enabled && usage.resident_blocks == 4 && usage.resident_unique_bytes == 0);
    source.forbid = 1;
    for (size_t i = 0; i < 1000; i++) {
        uint64_t id = (i * 37) % 4;
        int was_hit = 0;
        assert(cache_read_ex(cache, id, 8, bytes, 8, &was_hit) == 0 && was_hit == 1);
        for (size_t j = 0; j < 8; j++)
            assert(bytes[j] == (unsigned char)((id * 17 + 8 + j) % 251));
    }
    cache_stats_t stats = cache_get_stats(cache);
    assert(stats.requests == 1000 && stats.hits == 1000 && stats.misses == 0 &&
           stats.evictions == 0 && stats.load_errors == 0 && source.calls == 4);
    usage = cache_get_usage(cache);
    assert(usage.resident_unique_bytes == 32 && usage.evicted_blocks == 0);
    assert(cache_reset_observations(cache) == 0);
    assert_zero(cache);
    assert(cache_get_usage(cache).resident_unique_bytes == 0);
    int was_hit = 0;
    assert(cache_read_ex(cache, 3, 0, bytes, 8, &was_hit) == 0 && was_hit == 1);
    assert(source.calls == 4);
    cache_destroy(cache);
    puts("B0: warmed data preserved, 1000 measured hits, zero backend calls: OK");
}

static void test_reset_preserves_lru(void)
{
    source_t source = {0};
    unsigned char bytes[8];
    cache_t *cache = cache_create(2, 16, load, &source);
    assert(cache != NULL && cache_enable_usage(cache) == 0);
    assert(cache_read(cache, 0, 0, bytes, 8) == 0);
    assert(cache_read(cache, 1, 0, bytes, 8) == 0);
    assert(cache_read(cache, 0, 8, bytes, 8) == 0); /* MRU=0, LRU=1. */
    assert(cache_reset_observations(cache) == 0);
    assert(cache_read(cache, 2, 0, bytes, 1) == 0); /* Must evict 1. */
    source.forbid = 1;
    int was_hit = 0;
    assert(cache_read_ex(cache, 0, 0, bytes, 8, &was_hit) == 0 && was_hit == 1);
    assert(cache_read(cache, 1, 0, bytes, 8) == -1);
    cache_usage_stats_t usage = cache_get_usage(cache);
    assert(usage.evicted_blocks == 1 && usage.evicted_unique_bytes == 0);
    assert(usage.resident_blocks == 2 && usage.resident_unique_bytes == 9);
    cache_stats_t stats = cache_get_stats(cache);
    assert(stats.requests == 3 && stats.hits == 1 && stats.misses == 2 &&
           stats.load_errors == 1 && stats.evictions == 1);
    cache_destroy(cache);
    puts("Observation reset preserves LRU and excludes warmup byte coverage: OK");
}

static void test_empty_and_usage_disabled(void)
{
    source_t source = {0};
    unsigned char byte;
    assert(cache_reset_observations(NULL) == -1);
    cache_t *cache = cache_create(1, 8, load, &source);
    assert(cache != NULL && cache_reset_observations(cache) == 0);
    assert_zero(cache);
    assert(cache_read(cache, 0, 0, &byte, 1) == 0);
    assert(cache_reset_observations(cache) == 0);
    assert(cache_enable_usage(cache) == -1); /* Cannot attach coverage to a populated cache. */
    source.forbid = 1;
    assert(cache_read(cache, 0, 0, &byte, 1) == 0);
    assert(cache_get_stats(cache).hits == 1 && !cache_get_usage(cache).enabled);
    cache_destroy(cache);
    puts("Reset with empty cache and disabled instrumentation: OK");
}

int main(void)
{
    test_warm_then_hits();
    test_reset_preserves_lru();
    test_empty_and_usage_disabled();
    puts("All B0 cache tests passed");
    return 0;
}
