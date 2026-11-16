#ifndef CACHE_H
#define CACHE_H

#include <stddef.h>
#include <stdint.h>

typedef struct cache cache_t;

/* Return 0 after filling all block_size bytes; nonzero on failure.
 * The loader must not modify the destination beyond block_size bytes. */
typedef int (*cache_loader_fn)(void *context, uint64_t block_id,
                               void *destination, size_t block_size);

typedef struct {
    uint64_t requests;
    uint64_t hits;
    uint64_t misses;
    uint64_t load_errors;
    uint64_t evictions;
} cache_stats_t;

/* capacity_blocks counts payload slots. Metadata and one staging block
 * require additional memory. This cache is single-threaded. */
cache_t *cache_create(size_t capacity_blocks, size_t block_size,
                      cache_loader_fn loader, void *context);
void cache_destroy(cache_t *cache);

/* Copy bytes from one block; cross-block reads are rejected.
 * Returns 0 on success, -1 for invalid arguments or loader failure.
 * Invalid arguments do not affect statistics. A failed load counts as
 * a request and miss but preserves the existing contents and LRU order. */
int cache_read(cache_t *cache, uint64_t block_id, size_t offset,
               void *destination, size_t length);
cache_stats_t cache_get_stats(const cache_t *cache);

#endif
