#include "metrics.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void test_known_percentiles(void)
{
    latency_store_t store;
    latency_summary_t summary;
    assert(latency_store_init(&store, 100) == 0);
    for (uint64_t i = 100; i > 0; i--)
        assert(latency_record(&store, i * 10, i % 2 == 0, i % 2 ? i * 5 : 0) == 0);
    assert(latency_summarize(&store, LATENCY_ALL, &summary) == 0);
    assert(summary.count == 100 && summary.sum_ns == 50500 && summary.mean_ns == 505);
    assert(summary.min_ns == 10 && summary.p50_ns == 500 && summary.p95_ns == 950 &&
           summary.p99_ns == 990 && summary.max_ns == 1000);
    assert(latency_summarize(&store, LATENCY_HIT, &summary) == 0);
    assert(summary.count == 50 && summary.mean_ns == 510 && summary.p50_ns == 500 &&
           summary.p95_ns == 960 && summary.p99_ns == 1000);
    assert(latency_summarize(&store, LATENCY_MISS, &summary) == 0);
    assert(summary.count == 50 && summary.mean_ns == 500 && summary.p50_ns == 490 &&
           summary.p95_ns == 950 && summary.p99_ns == 990);
    assert(latency_summarize(&store, LATENCY_BACKEND, &summary) == 0);
    assert(summary.count == 50 && summary.mean_ns == 250 && summary.p50_ns == 245 &&
           summary.p95_ns == 475 && summary.p99_ns == 495);
    assert(latency_record(&store, 1, 1, 0) == -1);
    latency_store_destroy(&store);
    puts("Known means and nearest-rank p50/p95/p99: OK");
}

static void test_empty_and_invalid(void)
{
    latency_store_t store = {0};
    latency_summary_t summary;
    assert(latency_store_init(NULL, 2) == -1);
    assert(latency_store_init(&store, 0) == -1);
    assert(latency_store_init(&store, SIZE_MAX) == -1);
    assert(latency_store_init(&store, 2) == 0);
    assert(latency_summarize(&store, LATENCY_ALL, &summary) == 0 && summary.count == 0);
    assert(latency_record(&store, 10, 2, 0) == -1);
    assert(latency_record(&store, 10, 0, 11) == -1);
    assert(latency_record(&store, 10, 1, 1) == -1);
    assert(latency_record(&store, 20, 1, 0) == 0);
    assert(latency_summarize(&store, LATENCY_MISS, &summary) == 0 && summary.count == 0);
    assert(latency_summarize(&store, LATENCY_BACKEND, &summary) == 0 && summary.count == 0);
    assert(latency_record(&store, 10, 1, 0) == 0);
    assert(latency_summarize(&store, LATENCY_ALL, &summary) == 0);
    assert(summary.p50_ns == 10 && summary.p95_ns == 20 && summary.p99_ns == 20);
    assert(latency_summarize(&store, (latency_group_t)-1, &summary) == -1);
    latency_store_destroy(&store);
    latency_store_destroy(&store);
    assert(store.samples == NULL && store.count == 0);
    puts("Empty groups, small samples and invalid records: OK");
}

static void test_clock_arithmetic(void)
{
    uint64_t ns;
    assert(metrics_elapsed_ns((struct timespec){10, 900000000L},
                             (struct timespec){11, 100000000L}, &ns) == 0);
    assert(ns == 200000000);
    assert(metrics_elapsed_ns((struct timespec){10, 20}, (struct timespec){10, 20}, &ns) == 0);
    assert(ns == 0);
    assert(metrics_elapsed_ns((struct timespec){10, 20}, (struct timespec){10, 19}, &ns) == -1);
    assert(metrics_elapsed_ns((struct timespec){10, 0}, (struct timespec){9, 0}, &ns) == -1);
    assert(metrics_elapsed_ns((struct timespec){10, -1}, (struct timespec){11, 0}, &ns) == -1);
    assert(metrics_elapsed_ns((struct timespec){0, 0},
                             (struct timespec){0, 1000000000L}, &ns) == -1);
    assert(metrics_elapsed_ns((struct timespec){0, 0}, (struct timespec){1, 0}, NULL) == -1);
    puts("Nanosecond arithmetic and second-boundary carry: OK");
}

int main(void)
{
    test_known_percentiles();
    test_empty_and_invalid();
    test_clock_arithmetic();
    puts("All latency metrics tests passed");
    return 0;
}
