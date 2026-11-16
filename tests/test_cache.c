#include "cache.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>

typedef struct {
    size_t calls;
    int fail;
} source_t;

static unsigned char expected_byte(uint64_t id, size_t offset)
{
    return (unsigned char)((id * UINT64_C(37) + offset) & 255);
}

static int fake_load(void *context, uint64_t id, void *destination, size_t size)
{
    source_t *source = context;
    unsigned char *bytes = destination;
    source->calls++;
    /* Also touch staging on failure to test isolation of cached data. */
    for (size_t i = 0; i < size; i++)
        bytes[i] = expected_byte(id, i);
    return source->fail ? -1 : 0;
}

static void read_check(cache_t *cache, uint64_t id)
{
    unsigned char bytes[8];
    assert(cache_read(cache, id, 3, bytes, sizeof(bytes)) == 0);
    for (size_t i = 0; i < sizeof(bytes); i++)
        assert(bytes[i] == expected_byte(id, i + 3));
}

static void test_lru(void)
{
    source_t source = {0};
    cache_t *cache = cache_create(2, 16, fake_load, &source);
    assert(cache != NULL);
    const uint64_t trace[] = {1, 2, 1, 3, 1, 2};
    for (size_t i = 0; i < sizeof(trace) / sizeof(trace[0]); i++)
        read_check(cache, trace[i]);
    cache_stats_t stats = cache_get_stats(cache);
    assert(stats.requests == 6 && stats.hits == 2 && stats.misses == 4);
    assert(stats.evictions == 2 && stats.load_errors == 0 && source.calls == 4);
    printf("LRU: requests=%" PRIu64 " hits=%" PRIu64
           " misses=%" PRIu64 " evictions=%" PRIu64 "\n",
           stats.requests, stats.hits, stats.misses, stats.evictions);
    cache_destroy(cache);
}

static void test_failed_load(void)
{
    source_t source = {0};
    cache_t *cache = cache_create(2, 16, fake_load, &source);
    assert(cache != NULL);
    read_check(cache, 1);
    read_check(cache, 2);
    source.fail = 1;
    unsigned char byte = 123;
    assert(cache_read(cache, 99, 0, &byte, 1) == -1);
    assert(byte == 123);
    source.fail = 0;
    read_check(cache, 3); /* Should still evict 1, not 2. */
    read_check(cache, 2); /* Must hit and retain correct data. */
    read_check(cache, 1); /* Must miss. */
    cache_stats_t stats = cache_get_stats(cache);
    assert(stats.requests == 6 && stats.hits == 1 && stats.misses == 5);
    assert(stats.load_errors == 1 && stats.evictions == 2 && source.calls == 5);
    cache_destroy(cache);
    puts("Load failure: cached data and replacement order preserved");
}

static void test_arguments(void)
{
    source_t source = {0};
    assert(cache_create(0, 16, fake_load, &source) == NULL);
    assert(cache_create(1, 0, fake_load, &source) == NULL);
    assert(cache_create(1, 16, NULL, &source) == NULL);
    assert(cache_create(SIZE_MAX, 16, fake_load, &source) == NULL);
    cache_t *cache = cache_create(1, 16, fake_load, &source);
    assert(cache != NULL);
    unsigned char byte;
    assert(cache_read(cache, 1, 16, &byte, 1) == -1);
    assert(cache_read(cache, 1, SIZE_MAX, &byte, 1) == -1);
    assert(cache_read(cache, 1, 0, NULL, 1) == -1);
    assert(cache_read(cache, 1, 0, &byte, 0) == -1);
    assert(cache_read(NULL, 1, 0, &byte, 1) == -1);
    assert(cache_get_stats(cache).requests == 0 && source.calls == 0);
    assert(cache_read(cache, UINT64_MAX, 15, &byte, 1) == 0);
    assert(byte == expected_byte(UINT64_MAX, 15));
    read_check(cache, 0);
    read_check(cache, 0);
    assert(cache_get_stats(cache).evictions == 1);
    assert(cache_get_stats(cache).hits == 1);
    cache_destroy(cache);
    cache_destroy(NULL);
    puts("Arguments, boundary reads and capacity 1: OK");
}

static void test_reference(void)
{
    /* Independent, deliberately simple LRU reference based on timestamps. */
    enum { CAPACITY = 7, ACCESSES = 10000 };
    uint64_t ids[CAPACITY] = {0};
    size_t ages[CAPACITY] = {0};
    size_t used = 0, hits = 0, misses = 0;
    uint32_t random_state = 42;
    source_t source = {0};
    cache_t *cache = cache_create(CAPACITY, 16, fake_load, &source);
    assert(cache != NULL);
    for (size_t step = 1; step <= ACCESSES; step++) {
        random_state = random_state * UINT32_C(1664525) + UINT32_C(1013904223);
        uint64_t id = (random_state >> 16) % 23;
        size_t position = 0;
        while (position < used && ids[position] != id)
            position++;
        if (position < used) {
            hits++;
        } else {
            misses++;
            if (used < CAPACITY) {
                position = used++;
            } else {
                position = 0;
                for (size_t i = 1; i < used; i++)
                    if (ages[i] < ages[position])
                        position = i;
            }
            ids[position] = id;
        }
        ages[position] = step;
        read_check(cache, id);
        cache_stats_t stats = cache_get_stats(cache);
        assert(stats.hits == hits && stats.misses == misses);
        assert(stats.requests == step && stats.evictions == misses - used);
        assert(stats.load_errors == 0 && source.calls == misses);
    }
    cache_destroy(cache);
    puts("Independent LRU reference: 10000 accesses OK");
}

int main(void)
{
    test_lru();
    test_failed_load();
    test_arguments();
    test_reference();
    puts("All cache tests passed");
    return 0;
}
