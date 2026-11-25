#include "cache.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int load(void *context, uint64_t id, void *destination, size_t size)
{
    (void)context;
    memset(destination, (int)(id % 251), size);
    return id == 7 ? -1 : 0;
}

static void test_overlap_and_failure(void)
{
    cache_t *cache = cache_create(1, 16, load, NULL);
    unsigned char bytes[16];
    int hit = -1;
    assert(cache != NULL && cache_enable_usage(cache) == 0);
    assert(cache_read_ex(cache, 0, 0, bytes, 8, &hit) == 0 && hit == 0);
    assert(cache_read_ex(cache, 0, 0, bytes, 8, &hit) == 0 && hit == 1);
    assert(cache_read(cache, 0, 4, bytes, 8) == 0);
    cache_usage_stats_t usage = cache_get_usage(cache);
    assert(usage.resident_unique_bytes == 12 && usage.evicted_blocks == 0);
    assert(cache_read(cache, 1, 8, bytes, 8) == 0);
    usage = cache_get_usage(cache);
    assert(usage.evicted_blocks == 1 && usage.evicted_unique_bytes == 12);
    assert(usage.resident_blocks == 1 && usage.resident_unique_bytes == 8);
    assert(cache_read(cache, 7, 0, bytes, 8) == -1);
    assert(cache_read(cache, 1, 15, bytes, 2) == -1);
    usage = cache_get_usage(cache);
    assert(usage.evicted_blocks == 1 && usage.evicted_unique_bytes == 12);
    assert(usage.resident_unique_bytes == 8);
    assert(cache_read(cache, 1, 0, bytes, 16) == 0);
    usage = cache_get_usage(cache);
    assert(usage.resident_unique_bytes == 16);
    assert(cache_enable_usage(cache) == -1);
    cache_destroy(cache);
    puts("Unique byte overlap, eviction and failed-load preservation: OK");
}

static void test_disabled_and_tail(void)
{
    unsigned char bytes[9];
    cache_t *cache = cache_create(1, 9, load, NULL);
    assert(cache != NULL && !cache_get_usage(cache).enabled);
    assert(cache_read(cache, 0, 0, bytes, 9) == 0);
    assert(cache_enable_usage(cache) == -1);
    cache_destroy(cache);
    cache = cache_create(1, 9, load, NULL);
    assert(cache != NULL && cache_enable_usage(cache) == 0);
    assert(cache_enable_usage(cache) == 0);
    assert(cache_get_usage(cache).bitmap_bytes == 2);
    assert(cache_read(cache, 0, 8, bytes, 1) == 0);
    assert(cache_read(cache, 0, 0, bytes, 9) == 0);
    assert(cache_get_usage(cache).resident_unique_bytes == 9);
    cache_destroy(cache);
    assert(cache_enable_usage(NULL) == -1 && !cache_get_usage(NULL).enabled);
    puts("Optional instrumentation and non-multiple-of-eight block: OK");
}

static void test_independent_reference(void)
{
    /* Independent tiny MRU list and one boolean per byte (no bitmap). */
    int ids[2] = {-1, -1};
    unsigned char seen[2][17] = {{0}};
    size_t used = 0;
    uint64_t evictions = 0, evicted_bytes = 0;
    uint32_t state = 12345;
    unsigned char bytes[17];
    cache_t *cache = cache_create(2, 17, load, NULL);
    assert(cache != NULL && cache_enable_usage(cache) == 0);
    for (size_t step = 0; step < 3000; step++) {
        state = state * UINT32_C(1664525) + UINT32_C(1013904223);
        int id = (int)((state >> 16) % 8);
        size_t offset = (state >> 8) % 17;
        size_t length = 1 + state % (17 - offset);
        size_t index = 0;
        while (index < used && ids[index] != id)
            index++;
        int expected_hit = index < used;
        int hit = -1;
        int result = cache_read_ex(cache, (uint64_t)id, offset, bytes, length, &hit);
        if (id == 7) {
            assert(result == -1);
        } else {
            assert(result == 0 && hit == expected_hit);
            unsigned char current[17] = {0};
            if (expected_hit) {
                memcpy(current, seen[index], 17);
            } else if (used == 2) {
                for (size_t i = 0; i < 17; i++)
                    evicted_bytes += seen[1][i];
                evictions++;
                index = 1;
            } else {
                index = used++;
            }
            for (size_t i = index; i > 0; i--) {
                ids[i] = ids[i - 1];
                memcpy(seen[i], seen[i - 1], 17);
            }
            ids[0] = id;
            memcpy(seen[0], current, 17);
            for (size_t i = offset; i < offset + length; i++)
                seen[0][i] = 1;
        }
        uint64_t resident_bytes = 0;
        for (size_t j = 0; j < used; j++)
            for (size_t i = 0; i < 17; i++)
                resident_bytes += seen[j][i];
        cache_usage_stats_t usage = cache_get_usage(cache);
        assert(usage.evicted_blocks == evictions && usage.evicted_unique_bytes == evicted_bytes);
        assert(usage.resident_blocks == used && usage.resident_unique_bytes == resident_bytes);
    }
    cache_destroy(cache);
    puts("Independent byte-coverage/LRU reference: 3000 accesses OK");
}

int main(void)
{
    test_overlap_and_failure();
    test_disabled_and_tail();
    test_independent_reference();
    puts("All block utilization tests passed");
    return 0;
}
