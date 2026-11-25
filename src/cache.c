#define _POSIX_C_SOURCE 200809L
#include "cache.h"

#include <stdlib.h>
#include <string.h>

typedef struct entry {
    uint64_t id;
    unsigned char *data;
    unsigned char *coverage;
    size_t unique_bytes;
    struct entry *hash_next;
    struct entry *previous;
    struct entry *next;
} entry_t;

struct cache {
    size_t capacity, block_size, used, bucket_count;
    entry_t *entries;
    entry_t **buckets;
    unsigned char *data;
    unsigned char *staging;
    entry_t *most_recent;
    entry_t *least_recent;
    cache_loader_fn loader;
    void *context;
    cache_stats_t stats;
    unsigned char *coverage;
    size_t coverage_stride, coverage_bytes;
    uint64_t evicted_unique_bytes;
};

static size_t bucket_for(const cache_t *cache, uint64_t id)
{
    /* Mix the ID before choosing a hash bucket. */
    id ^= id >> 30;
    id *= UINT64_C(0xbf58476d1ce4e5b9);
    id ^= id >> 27;
    id *= UINT64_C(0x94d049bb133111eb);
    id ^= id >> 31;
    return (size_t)(id % cache->bucket_count);
}

static entry_t *lookup(const cache_t *cache, uint64_t id)
{
    for (entry_t *entry = cache->buckets[bucket_for(cache, id)];
         entry != NULL; entry = entry->hash_next) {
        if (entry->id == id)
            return entry;
    }
    return NULL;
}

static void detach(cache_t *cache, entry_t *entry)
{
    if (entry->previous != NULL)
        entry->previous->next = entry->next;
    else
        cache->most_recent = entry->next;
    if (entry->next != NULL)
        entry->next->previous = entry->previous;
    else
        cache->least_recent = entry->previous;
}

static void prepend(cache_t *cache, entry_t *entry)
{
    entry->previous = NULL;
    entry->next = cache->most_recent;
    if (cache->most_recent != NULL)
        cache->most_recent->previous = entry;
    else
        cache->least_recent = entry;
    cache->most_recent = entry;
}

static void hash_remove(cache_t *cache, entry_t *entry)
{
    entry_t **link = &cache->buckets[bucket_for(cache, entry->id)];
    while (*link != entry)
        link = &(*link)->hash_next;
    *link = entry->hash_next;
}

cache_t *cache_create(size_t capacity_blocks, size_t block_size,
                      cache_loader_fn loader, void *context)
{
    if (capacity_blocks == 0 || block_size == 0 || loader == NULL ||
        capacity_blocks > SIZE_MAX / block_size ||
        capacity_blocks > SIZE_MAX / sizeof(entry_t) ||
        capacity_blocks > (SIZE_MAX - 1) / 2)
        return NULL;

    size_t bucket_count = capacity_blocks * 2 + 1;
    if (bucket_count > SIZE_MAX / sizeof(entry_t *))
        return NULL;

    cache_t *cache = calloc(1, sizeof(*cache));
    if (cache == NULL)
        return NULL;
    cache->capacity = capacity_blocks;
    cache->block_size = block_size;
    cache->bucket_count = bucket_count;
    cache->loader = loader;
    cache->context = context;
    cache->entries = calloc(capacity_blocks, sizeof(entry_t));
    cache->buckets = calloc(bucket_count, sizeof(entry_t *));

    /* Aligned staging accommodates the planned Linux direct-I/O backend.
     * Device-specific offset/length alignment must still be checked there. */
    void *data = NULL;
    void *staging = NULL;
    int data_error = posix_memalign(&data, 4096, capacity_blocks * block_size);
    int staging_error = posix_memalign(&staging, 4096, block_size);
    cache->data = data;
    cache->staging = staging;
    if (cache->entries == NULL || cache->buckets == NULL ||
        data_error != 0 || staging_error != 0) {
        cache_destroy(cache);
        return NULL;
    }
    return cache;
}

void cache_destroy(cache_t *cache)
{
    if (cache == NULL)
        return;
    free(cache->coverage);
    free(cache->staging);
    free(cache->data);
    free(cache->buckets);
    free(cache->entries);
    free(cache);
}

int cache_enable_usage(cache_t *cache)
{
    if (cache == NULL || cache->stats.requests != 0 || cache->used != 0)
        return -1;
    if (cache->coverage != NULL)
        return 0;
    size_t stride = cache->block_size / 8 + (cache->block_size % 8 != 0);
    if (cache->capacity > SIZE_MAX / stride)
        return -1;
    size_t bytes = stride * cache->capacity;
    unsigned char *coverage = calloc(bytes, 1);
    if (coverage == NULL)
        return -1;
    /* Touch every allocated page before the measured loop. */
    volatile unsigned char *touch = coverage;
    for (size_t i = 0; i < bytes; ) {
        touch[i] = 0;
        if (bytes - i <= 4096)
            break;
        i += 4096;
    }
    touch[bytes - 1] = 0;
    cache->coverage = coverage;
    cache->coverage_stride = stride;
    cache->coverage_bytes = bytes;
    return 0;
}

static void record_coverage(entry_t *entry, size_t offset, size_t length)
{
    if (entry->coverage == NULL)
        return;
    for (size_t i = offset; i < offset + length; i++) {
        unsigned char mask = (unsigned char)(1u << (i % 8));
        if ((entry->coverage[i / 8] & mask) == 0) {
            entry->coverage[i / 8] |= mask;
            entry->unique_bytes++;
        }
    }
}

int cache_read_ex(cache_t *cache, uint64_t block_id, size_t offset,
                  void *destination, size_t length, int *was_hit)
{
    if (cache == NULL || destination == NULL || length == 0 ||
        offset > cache->block_size || length > cache->block_size - offset)
        return -1;

    cache->stats.requests++;
    entry_t *entry = lookup(cache, block_id);
    if (was_hit != NULL)
        *was_hit = entry != NULL;
    if (entry != NULL) {
        cache->stats.hits++;
        detach(cache, entry);
    } else {
        cache->stats.misses++;
        if (cache->loader(cache->context, block_id, cache->staging,
                          cache->block_size) != 0) {
            cache->stats.load_errors++;
            return -1;
        }
        if (cache->used < cache->capacity) {
            entry = &cache->entries[cache->used];
            entry->data = cache->data + cache->used * cache->block_size;
            if (cache->coverage != NULL)
                entry->coverage = cache->coverage + cache->used * cache->coverage_stride;
            cache->used++;
        } else {
            entry = cache->least_recent;
            if (cache->coverage != NULL)
                cache->evicted_unique_bytes += entry->unique_bytes;
            hash_remove(cache, entry);
            detach(cache, entry);
            cache->stats.evictions++;
        }
        entry->id = block_id;
        entry->unique_bytes = 0;
        if (entry->coverage != NULL)
            memset(entry->coverage, 0, cache->coverage_stride);
        memcpy(entry->data, cache->staging, cache->block_size);
        size_t bucket = bucket_for(cache, block_id);
        entry->hash_next = cache->buckets[bucket];
        cache->buckets[bucket] = entry;
    }
    prepend(cache, entry);
    memcpy(destination, entry->data + offset, length);
    record_coverage(entry, offset, length);
    return 0;
}

int cache_read(cache_t *cache, uint64_t block_id, size_t offset,
               void *destination, size_t length)
{
    return cache_read_ex(cache, block_id, offset, destination, length, NULL);
}

cache_usage_stats_t cache_get_usage(const cache_t *cache)
{
    cache_usage_stats_t result = {0};
    if (cache == NULL || cache->coverage == NULL)
        return result;
    result.enabled = 1;
    result.bitmap_bytes = cache->coverage_bytes;
    result.evicted_blocks = cache->stats.evictions;
    result.evicted_unique_bytes = cache->evicted_unique_bytes;
    result.resident_blocks = cache->used;
    for (size_t i = 0; i < cache->used; i++)
        result.resident_unique_bytes += cache->entries[i].unique_bytes;
    return result;
}

cache_stats_t cache_get_stats(const cache_t *cache)
{
    return cache != NULL ? cache->stats : (cache_stats_t){0};
}

int cache_reset_observations(cache_t *cache)
{
    if (cache == NULL)
        return -1;
    cache->stats = (cache_stats_t){0};
    cache->evicted_unique_bytes = 0;
    if (cache->coverage != NULL)
        memset(cache->coverage, 0, cache->coverage_bytes);
    for (size_t i = 0; i < cache->used; i++)
        cache->entries[i].unique_bytes = 0;
    return 0;
}
