#include "metrics.h"

#include <stdlib.h>

int latency_store_init(latency_store_t *store, size_t capacity)
{
    if (store == NULL)
        return -1;
    *store = (latency_store_t){0};
    if (capacity == 0 || capacity > SIZE_MAX / sizeof(latency_sample_t))
        return -1;
    size_t bytes = capacity * sizeof(latency_sample_t);
    store->samples = malloc(bytes);
    if (store->samples == NULL)
        return -1;
    volatile unsigned char *touch = (volatile unsigned char *)store->samples;
    for (size_t i = 0; i < bytes; ) {
        touch[i] = 0;
        if (bytes - i <= 4096)
            break;
        i += 4096;
    }
    touch[bytes - 1] = 0;
    store->capacity = capacity;
    return 0;
}

void latency_store_destroy(latency_store_t *store)
{
    if (store != NULL) {
        free(store->samples);
        *store = (latency_store_t){0};
    }
}

int latency_record(latency_store_t *store, uint64_t access_ns,
                    int was_hit, uint64_t backend_ns)
{
    if (store == NULL || store->samples == NULL || store->count >= store->capacity ||
        (was_hit != 0 && was_hit != 1) || backend_ns > access_ns ||
        (was_hit && backend_ns != 0))
        return -1;
    store->samples[store->count++] = (latency_sample_t){access_ns, backend_ns, was_hit};
    return 0;
}

static int selected(const latency_sample_t *sample, latency_group_t group)
{
    return group == LATENCY_ALL || (group == LATENCY_HIT && sample->was_hit) ||
           ((group == LATENCY_MISS || group == LATENCY_BACKEND) && !sample->was_hit);
}

static int compare_ns(const void *first, const void *second)
{
    uint64_t a = *(const uint64_t *)first, b = *(const uint64_t *)second;
    return (a > b) - (a < b);
}

static size_t percentile_index(size_t count, size_t percent)
{
    /* ceil(count*percent/100)-1 without overflowing count*percent. */
    size_t rank = (count / 100) * percent + ((count % 100) * percent + 99) / 100;
    return rank - 1;
}

int latency_summarize(const latency_store_t *store, latency_group_t group,
                       latency_summary_t *summary)
{
    if (store == NULL || store->samples == NULL || summary == NULL ||
        (unsigned)group > (unsigned)LATENCY_BACKEND)
        return -1;
    *summary = (latency_summary_t){0};
    for (size_t i = 0; i < store->count; i++)
        if (selected(&store->samples[i], group))
            summary->count++;
    if (summary->count == 0)
        return 0;
    /* count <= capacity, whose larger sample allocation was checked. */
    uint64_t *sorted = malloc(summary->count * sizeof(uint64_t));
    if (sorted == NULL)
        return -1;
    size_t next = 0;
    for (size_t i = 0; i < store->count; i++) {
        const latency_sample_t *sample = &store->samples[i];
        if (selected(sample, group)) {
            uint64_t ns = group == LATENCY_BACKEND ? sample->backend_ns : sample->access_ns;
            sorted[next++] = ns;
            summary->sum_ns += (long double)ns;
        }
    }
    qsort(sorted, summary->count, sizeof(*sorted), compare_ns);
    summary->mean_ns = summary->sum_ns / (long double)summary->count;
    summary->min_ns = sorted[0];
    summary->max_ns = sorted[summary->count - 1];
    summary->p50_ns = sorted[percentile_index(summary->count, 50)];
    summary->p95_ns = sorted[percentile_index(summary->count, 95)];
    summary->p99_ns = sorted[percentile_index(summary->count, 99)];
    free(sorted);
    return 0;
}

int metrics_elapsed_ns(struct timespec start, struct timespec end, uint64_t *ns)
{
    if (ns == NULL || start.tv_sec < 0 || end.tv_sec < start.tv_sec ||
        start.tv_nsec < 0 || start.tv_nsec >= 1000000000L ||
        end.tv_nsec < 0 || end.tv_nsec >= 1000000000L)
        return -1;
    uint64_t seconds = (uint64_t)end.tv_sec - (uint64_t)start.tv_sec;
    long nanos = end.tv_nsec - start.tv_nsec;
    if (nanos < 0) {
        if (seconds == 0)
            return -1;
        seconds--;
        nanos += 1000000000L;
    }
    if (seconds > (UINT64_MAX - (uint64_t)nanos) / UINT64_C(1000000000))
        return -1;
    *ns = seconds * UINT64_C(1000000000) + (uint64_t)nanos;
    return 0;
}
