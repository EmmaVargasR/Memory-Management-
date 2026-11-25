#ifndef METRICS_H
#define METRICS_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    uint64_t access_ns, backend_ns;
    int was_hit;
} latency_sample_t;

typedef struct {
    latency_sample_t *samples;
    size_t capacity, count;
} latency_store_t;

typedef enum { LATENCY_ALL, LATENCY_HIT, LATENCY_MISS, LATENCY_BACKEND } latency_group_t;

typedef struct {
    size_t count;
    long double sum_ns, mean_ns;
    uint64_t min_ns, p50_ns, p95_ns, p99_ns, max_ns;
} latency_summary_t;

/* Exact individual samples, no sampling or histogram approximation.
 * Allocate and prefault before timing; summarize only after timing.
 * Percentiles use nearest rank ceil(p*n), with ranks starting at 1.
 * Empty groups have count=0; their zero numeric fields are not observations. */
int latency_store_init(latency_store_t *store, size_t capacity);
void latency_store_destroy(latency_store_t *store);
int latency_record(latency_store_t *store, uint64_t access_ns,
                    int was_hit, uint64_t backend_ns);
int latency_summarize(const latency_store_t *store, latency_group_t group,
                       latency_summary_t *summary);
int metrics_elapsed_ns(struct timespec start, struct timespec end, uint64_t *ns);

#endif
