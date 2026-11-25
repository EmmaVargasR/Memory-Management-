#define _POSIX_C_SOURCE 200809L
#include "cache.h"
#include "metrics.h"
#include "workload.h"
#include "backend_tcp.h"
#include "backend_sd.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    size_t block_size;
    size_t cache_bytes;
    size_t working_set_bytes;
    size_t accesses;
    const char *backend;
    const char *host;
    size_t port;
    const char *file;
    const char *pattern;
    uint64_t seed;
    double theta;
    int theta_given;
    int metrics;
    const char *latency_csv;
    int b0;
} options_t;

static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
        "Usage: %s [--block-size BYTES] [--cache-size BYTES]\n"
        "          [--working-set BYTES] [--accesses COUNT]\n"
        "          [--backend synthetic|tcp|sd] [--host IPv4] [--port PORT]\n"
        "          [--file DATASET] (required only for --backend sd)\n"
        "          [--pattern sequential|uniform|zipf] [--seed UINT64]\n"
        "          [--zipf-theta VALUE] (0 < VALUE <= 2, zipf only)\n"
        "          [--metrics on|off] [--latency-csv NEW_FILE] (metrics on only)\n"
        "          [--baseline none|b0] (default none; b0 requires cache=working-set)\n"
        "Metrics default=on; exact samples and byte coverage add overhead.\n"
        "Sizes are positive decimal bytes, without unit suffixes.\n"
        "Defaults: block=4096 cache=33554432 working-set=268435456 accesses=1000000\n"
        "Default backend=synthetic host=127.0.0.1 port=18083.\n"
        "Default pattern=sequential seed=42 zipf-theta=1.1.\n"
        "Aligned 8-byte reads; empty cache unless --baseline b0 warms all blocks.\n",
        program);
}

static int parse_seed(const char *text, uint64_t *seed)
{
    if (*text == '\0')
        return -1;
    for (const char *p = text; *p; p++)
        if (*p < '0' || *p > '9')
            return -1;
    errno = 0;
    char *end = NULL;
    uintmax_t value = strtoumax(text, &end, 10);
    if (errno != 0 || *end != '\0' || value > UINT64_MAX)
        return -1;
    *seed = (uint64_t)value;
    return 0;
}

static int positive_size(const char *text, size_t *value)
{
    if (*text == '\0')
        return -1;
    for (const char *p = text; *p != '\0'; p++)
        if (*p < '0' || *p > '9')
            return -1;
    errno = 0;
    char *end = NULL;
    uintmax_t parsed = strtoumax(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed == 0 || parsed > SIZE_MAX)
        return -1;
    *value = (size_t)parsed;
    return 0;
}

static int parse_options(int argc, char **argv, options_t *options)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--baseline") == 0) {
            if (i + 1 >= argc || (strcmp(argv[i + 1], "none") != 0 &&
                                  strcmp(argv[i + 1], "b0") != 0)) {
                fputs("Invalid baseline: use none or b0.\n", stderr);
                return -1;
            }
            options->b0 = strcmp(argv[++i], "b0") == 0;
            continue;
        }
        if (strcmp(argv[i], "--metrics") == 0) {
            if (i + 1 >= argc || (strcmp(argv[i + 1], "on") != 0 &&
                                  strcmp(argv[i + 1], "off") != 0)) {
                fputs("Invalid metrics mode: use on or off.\n", stderr);
                return -1;
            }
            options->metrics = strcmp(argv[++i], "on") == 0;
            continue;
        }
        if (strcmp(argv[i], "--latency-csv") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                fputs("--latency-csv requires a new file path.\n", stderr);
                return -1;
            }
            options->latency_csv = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--seed") == 0) {
            if (i + 1 >= argc || parse_seed(argv[++i], &options->seed) != 0) {
                fputs("Invalid seed: use a decimal uint64, including zero.\n", stderr);
                return -1;
            }
            continue;
        }
        if (strcmp(argv[i], "--zipf-theta") == 0) {
            if (i + 1 >= argc) {
                fputs("Invalid Zipf theta: missing value.\n", stderr);
                return -1;
            }
            const char *value = argv[++i];
            char *end = NULL;
            errno = 0;
            options->theta = strtod(value, &end);
            if (*value == '\0' || *end != '\0' || errno != 0 ||
                !isfinite(options->theta) || options->theta <= 0.0 || options->theta > 2.0) {
                fputs("Invalid Zipf theta: require 0 < theta <= 2.\n", stderr);
                return -1;
            }
            options->theta_given = 1;
            continue;
        }
        if (strcmp(argv[i], "--backend") == 0 || strcmp(argv[i], "--host") == 0 ||
            strcmp(argv[i], "--file") == 0 || strcmp(argv[i], "--pattern") == 0) {
            if (i + 1 >= argc) {
                fputs("Missing backend, host, file or pattern value.\n", stderr);
                return -1;
            }
            if (strcmp(argv[i], "--backend") == 0)
                options->backend = argv[++i];
            else if (strcmp(argv[i], "--host") == 0)
                options->host = argv[++i];
            else if (strcmp(argv[i], "--pattern") == 0)
                options->pattern = argv[++i];
            else
                options->file = argv[++i];
            continue;
        }
        size_t *destination = NULL;
        if (strcmp(argv[i], "--block-size") == 0)
            destination = &options->block_size;
        else if (strcmp(argv[i], "--cache-size") == 0)
            destination = &options->cache_bytes;
        else if (strcmp(argv[i], "--working-set") == 0)
            destination = &options->working_set_bytes;
        else if (strcmp(argv[i], "--accesses") == 0)
            destination = &options->accesses;
        else if (strcmp(argv[i], "--port") == 0)
            destination = &options->port;
        if (destination == NULL || i + 1 >= argc) {
            fprintf(stderr, "Unknown option or missing value: %s\n", argv[i]);
            return -1;
        }
        if (positive_size(argv[++i], destination) != 0) {
            fprintf(stderr, "Invalid positive integer: %s\n", argv[i]);
            return -1;
        }
    }
    if (options->b0 && options->cache_bytes != options->working_set_bytes) {
        fputs("B0 requires cache-size equal to working-set (100% payload).\n", stderr);
        return -1;
    }
    if (!options->metrics && options->latency_csv != NULL) {
        fputs("--latency-csv requires --metrics on.\n", stderr);
        return -1;
    }
    if (options->port > 65535 ||
        (strcmp(options->backend, "synthetic") != 0 &&
         strcmp(options->backend, "tcp") != 0 && strcmp(options->backend, "sd") != 0)) {
        fputs("Invalid port or backend.\n", stderr);
        return -1;
    }
    if (options->block_size < 8 || options->block_size % 8 != 0 ||
        options->cache_bytes % options->block_size != 0 ||
        options->working_set_bytes % options->block_size != 0) {
        fputs("Block size must be a multiple of 8; cache and working set must "
              "be positive multiples of block size.\n", stderr);
        return -1;
    }
    if (strcmp(options->backend, "sd") == 0) {
        if (options->file == NULL || options->file[0] == '\0') {
            fputs("--file is required for --backend sd.\n", stderr);
            return -1;
        }
        if (options->block_size < 4096 || options->block_size % 4096 != 0) {
            fputs("SD block size must be divisible by 4096 and at least 4096.\n", stderr);
            return -1;
        }
    } else if (options->file != NULL) {
        fputs("--file requires --backend sd.\n", stderr);
        return -1;
    }
    if (strcmp(options->pattern, "sequential") != 0 &&
        strcmp(options->pattern, "uniform") != 0 && strcmp(options->pattern, "zipf") != 0) {
        fputs("Invalid pattern: use sequential, uniform or zipf.\n", stderr);
        return -1;
    }
    if (options->theta_given && strcmp(options->pattern, "zipf") != 0) {
        fputs("--zipf-theta requires --pattern zipf.\n", stderr);
        return -1;
    }
    return 0;
}

static int synthetic_load(void *context, uint64_t block_id,
                          void *destination, size_t block_size)
{
    const options_t *options = context;
    if (block_size != options->block_size ||
        block_id >= options->working_set_bytes / block_size)
        return -1;
    size_t start = (size_t)block_id * block_size;
    unsigned char *bytes = destination;
    /* Data depends on absolute byte address, not on the chosen block size. */
    for (size_t i = 0; i < block_size; i++)
        bytes[i] = (unsigned char)((start + i) % 251);
    return 0;
}

static double elapsed_seconds(struct timespec start, struct timespec end)
{
    return (double)(end.tv_sec - start.tv_sec) +
           (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
}

static void close_backends(tcp_backend_t *tcp, sd_backend_t *sd)
{
    tcp_backend_close(tcp);
    sd_backend_close(sd);
}

typedef struct {
    cache_loader_fn load;
    void *context;
    uint64_t last_ns;
} timed_backend_t;

static int timed_load(void *context, uint64_t block_id, void *destination, size_t block_size)
{
    timed_backend_t *backend = context;
    struct timespec start, end;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &start) != 0)
        return -1;
    int result = backend->load(backend->context, block_id, destination, block_size);
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &end) != 0 ||
        metrics_elapsed_ns(start, end, &backend->last_ns) != 0)
        return -1;
    return result;
}

static int export_latencies(const latency_store_t *store, const char *path)
{
    FILE *file = fopen(path, "wx");
    if (file == NULL) {
        perror("Create latency CSV (existing files are not overwritten)");
        return -1;
    }
    int failed = fputs("access_index,was_hit,access_ns,backend_ns\n", file) == EOF;
    for (size_t i = 0; i < store->count && !failed; i++) {
        const latency_sample_t *sample = &store->samples[i];
        failed = fprintf(file, "%zu,%d,%" PRIu64 ",%" PRIu64 "\n", i,
                         sample->was_hit, sample->access_ns, sample->backend_ns) < 0;
    }
    if (fclose(file) != 0)
        failed = 1;
    if (failed) {
        fputs("Could not finish latency CSV; partial file removed.\n", stderr);
        (void)remove(path);
        return -1;
    }
    return 0;
}

static void print_fraction(const char *name, uint64_t bytes, uint64_t blocks, size_t size)
{
    if (blocks == 0)
        printf("%s=na\n", name);
    else
        printf("%s=%.9Lf\n", name, (long double)bytes / ((long double)blocks * size));
}

static int print_metrics(const latency_store_t *store, const cache_t *cache, size_t block_size)
{
    const char *names[] = {"all", "hit", "miss", "backend"};
    latency_summary_t summaries[4];
    for (size_t i = 0; i < 4; i++)
        if (latency_summarize(store, (latency_group_t)i, &summaries[i]) != 0) {
            fputs("Could not allocate latency summary scratch memory.\n", stderr);
            return -1;
        }
    puts("metrics_version=latency-usage-v1\nlatency_clock=CLOCK_MONOTONIC_RAW\nlatency_unit=ns\n"
         "latency_percentile_method=nearest_rank\n"
         "latency_scope=cache_read_including_usage_and_timing_instrumentation\n"
         "backend_latency_scope=loader_callback_excluding_cache_copy_and_lru\n"
         "latency_overhead_subtracted=0");
    printf("metrics_sample_bytes=%zu\nlatency_summary_scratch_bytes=%zu\n",
           store->capacity * sizeof(latency_sample_t), store->count * sizeof(uint64_t));
    for (size_t i = 0; i < 4; i++) {
        latency_summary_t *summary = &summaries[i];
        printf("latency_%s_count=%zu\n", names[i], summary->count);
        if (summary->count == 0) {
            printf("latency_%s_sum_ns=na\nlatency_%s_mean_ns=na\n"
                   "latency_%s_min_ns=na\nlatency_%s_p50_ns=na\n"
                   "latency_%s_p95_ns=na\nlatency_%s_p99_ns=na\nlatency_%s_max_ns=na\n",
                   names[i], names[i], names[i], names[i], names[i], names[i], names[i]);
        } else {
            printf("latency_%s_sum_ns=%.0Lf\nlatency_%s_mean_ns=%.3Lf\n",
                   names[i], summary->sum_ns, names[i], summary->mean_ns);
            printf("latency_%s_min_ns=%" PRIu64 "\nlatency_%s_p50_ns=%" PRIu64
                   "\nlatency_%s_p95_ns=%" PRIu64 "\nlatency_%s_p99_ns=%" PRIu64
                   "\nlatency_%s_max_ns=%" PRIu64 "\n",
                   names[i], summary->min_ns, names[i], summary->p50_ns,
                   names[i], summary->p95_ns, names[i], summary->p99_ns,
                   names[i], summary->max_ns);
        }
    }
    cache_usage_stats_t usage = cache_get_usage(cache);
    puts("block_usage_unit=distinct_bytes_per_residency\n"
         "block_usage_resident_scope=partial_lifetimes_at_end");
    printf("usage_bitmap_bytes=%zu\nusage_evicted_blocks=%" PRIu64
           "\nusage_evicted_unique_bytes=%" PRIu64 "\nusage_resident_blocks=%" PRIu64
           "\nusage_resident_unique_bytes=%" PRIu64 "\n",
           usage.bitmap_bytes, usage.evicted_blocks, usage.evicted_unique_bytes,
           usage.resident_blocks, usage.resident_unique_bytes);
    print_fraction("block_utilization_evicted", usage.evicted_unique_bytes,
                   usage.evicted_blocks, block_size);
    print_fraction("block_utilization_resident", usage.resident_unique_bytes,
                   usage.resident_blocks, block_size);
    print_fraction("block_utilization_all_loaded",
                   usage.evicted_unique_bytes + usage.resident_unique_bytes,
                   usage.evicted_blocks + usage.resident_blocks, block_size);
    return 0;
}

int main(int argc, char **argv)
{
    options_t options = {4096, 33554432, 268435456, 1000000,
                          "synthetic", "127.0.0.1", 18083, NULL,
                          "sequential", 42, 1.1, 0, 1, NULL, 0};
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout, argv[0]);
        return EXIT_SUCCESS;
    }
    if (parse_options(argc, argv, &options) != 0) {
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }

    workload_t workload;
    if (workload_init(&workload, options.pattern, options.working_set_bytes,
                       options.seed, options.theta) != 0) {
        fputs("Invalid workload configuration; Zipf supports at most 2^48 logical words.\n", stderr);
        return EXIT_FAILURE;
    }
    tcp_backend_t tcp = {.fd = -1};
    sd_backend_t sd = {.fd = -1};
    cache_loader_fn loader = synthetic_load;
    void *loader_context = &options;
    int use_tcp = strcmp(options.backend, "tcp") == 0;
    int use_sd = strcmp(options.backend, "sd") == 0;
    if (use_tcp) {
        if (tcp_backend_open(&tcp, options.host, (uint16_t)options.port,
                             options.block_size, options.working_set_bytes) != 0)
            return EXIT_FAILURE;
        loader = tcp_backend_load;
        loader_context = &tcp;
    } else if (use_sd) {
        if (sd_backend_open(&sd, options.file, options.block_size,
                              options.working_set_bytes) != 0)
            return EXIT_FAILURE;
        loader = sd_backend_load;
        loader_context = &sd;
    }
    timed_backend_t timed = {.load = loader, .context = loader_context};
    if (options.metrics) {
        loader = timed_load;
        loader_context = &timed;
    }
    latency_store_t latencies = {0};
    cache_t *cache = cache_create(options.cache_bytes / options.block_size,
                                  options.block_size, loader, loader_context);
    if (cache == NULL) {
        fputs("Could not allocate cache.\n", stderr);
        close_backends(&tcp, &sd);
        return EXIT_FAILURE;
    }

    int exit_code = EXIT_FAILURE;
    if (options.metrics && (cache_enable_usage(cache) != 0 ||
                           latency_store_init(&latencies, options.accesses) != 0)) {
        fputs("Could not allocate metrics memory; reduce accesses or cache size.\n", stderr);
        goto cleanup;
    }
    cache_stats_t warmup = {0};
    double warmup_seconds = 0.0;
    if (options.b0) {
        struct timespec warm_start, warm_end;
        if (clock_gettime(CLOCK_MONOTONIC_RAW, &warm_start) != 0) {
            perror("Warmup clock");
            goto cleanup;
        }
        size_t blocks = options.working_set_bytes / options.block_size;
        for (size_t id = 0; id < blocks; id++) {
            unsigned char bytes[8];
            if (cache_read(cache, (uint64_t)id, 0, bytes, sizeof(bytes)) != 0) {
                fprintf(stderr, "B0 warmup failed at block %zu; measurement not started.\n", id);
                goto cleanup;
            }
        }
        if (clock_gettime(CLOCK_MONOTONIC_RAW, &warm_end) != 0) {
            perror("Warmup clock");
            goto cleanup;
        }
        warmup_seconds = elapsed_seconds(warm_start, warm_end);
        warmup = cache_get_stats(cache);
        if (warmup.requests != blocks || warmup.misses != blocks || warmup.hits != 0 ||
            warmup.evictions != 0 || warmup.load_errors != 0 ||
            cache_reset_observations(cache) != 0) {
            fputs("Invalid B0 warmup state; measurement not started.\n", stderr);
            goto cleanup;
        }
    }
    /* Snapshot backend counters; their state and connection stay intact.
     * Workload state and latency samples were not touched during warmup. */
    uint64_t warm_tcp_requests = tcp.requests, warm_tcp_bytes = tcp.payload_bytes;
    uint64_t warm_sd_requests = sd.requests, warm_sd_calls = sd.read_calls, warm_sd_bytes = sd.read_bytes;
    struct timespec start, end;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &start) != 0) {
        perror("clock_gettime");
        goto cleanup;
    }
    uint64_t checksum = 0;
    uint64_t trace_hash = UINT64_C(14695981039346656037);
    uint64_t hot20_accesses = 0;
    size_t hot20_items = workload.items / 5 + (workload.items % 5 != 0);
    for (size_t i = 0; i < options.accesses; i++) {
        size_t address = workload_next(&workload);
        trace_hash = (trace_hash ^ (uint64_t)address) * UINT64_C(1099511628211);
        if (address / 8 < hot20_items)
            hot20_accesses++;
        uint64_t block_id = (uint64_t)(address / options.block_size);
        size_t offset = address % options.block_size;
        unsigned char bytes[8];
        int result;
        if (options.metrics) {
            struct timespec access_start, access_end;
            int was_hit = 0;
            uint64_t ns;
            timed.last_ns = 0;
            if (clock_gettime(CLOCK_MONOTONIC_RAW, &access_start) != 0) {
                perror("Access clock");
                goto cleanup;
            }
            result = cache_read_ex(cache, block_id, offset, bytes, sizeof(bytes), &was_hit);
            if (clock_gettime(CLOCK_MONOTONIC_RAW, &access_end) != 0 ||
                metrics_elapsed_ns(access_start, access_end, &ns) != 0) {
                fputs("Invalid access clock measurement.\n", stderr);
                goto cleanup;
            }
            if (result == 0 && latency_record(&latencies, ns, was_hit, timed.last_ns) != 0) {
                fputs("Could not record access latency.\n", stderr);
                goto cleanup;
            }
        } else {
            result = cache_read(cache, block_id, offset, bytes, sizeof(bytes));
        }
        if (result != 0) {
            fprintf(stderr, "Read failed at access %zu.\n", i);
            goto cleanup;
        }
        /* Defined unsigned wraparound; independent of machine byte order. */
        for (size_t j = 0; j < sizeof(bytes); j++)
            checksum += bytes[j];
    }
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &end) != 0) {
        perror("clock_gettime");
        goto cleanup;
    }
    cache_stats_t stats = cache_get_stats(cache);
    if (options.b0 && (stats.requests != options.accesses || stats.hits != options.accesses ||
                      stats.misses != 0 || stats.evictions != 0 || stats.load_errors != 0 ||
                      tcp.requests != warm_tcp_requests || tcp.payload_bytes != warm_tcp_bytes ||
                      sd.requests != warm_sd_requests || sd.read_calls != warm_sd_calls ||
                      sd.read_bytes != warm_sd_bytes)) {
        fputs("B0 validation failed: measurement must have only hits and no backend I/O.\n", stderr);
        goto cleanup;
    }
    if (options.latency_csv != NULL && export_latencies(&latencies, options.latency_csv) != 0)
        goto cleanup;
    double seconds = elapsed_seconds(start, end);
    printf("baseline=%s\nb0_verified=%s\n", options.b0 ? "b0" : "none",
           options.b0 ? "1" : "na");
    printf("warmup_requests=%" PRIu64 "\nwarmup_hits=%" PRIu64 "\nwarmup_misses=%" PRIu64
           "\nwarmup_evictions=%" PRIu64 "\nwarmup_load_errors=%" PRIu64
           "\nwarmup_elapsed_seconds=%.9f\n", warmup.requests, warmup.hits, warmup.misses,
           warmup.evictions, warmup.load_errors, warmup_seconds);
    printf("warmup_tcp_requests=%" PRIu64 "\nwarmup_tcp_payload_bytes=%" PRIu64
           "\nwarmup_sd_requests=%" PRIu64 "\nwarmup_sd_read_calls=%" PRIu64
           "\nwarmup_sd_read_bytes=%" PRIu64 "\n", warm_tcp_requests, warm_tcp_bytes,
           warm_sd_requests, warm_sd_calls, warm_sd_bytes);
    puts("backend_request_counters_scope=measurement_only\ntcp_connections_scope=whole_process");
    printf("backend=%s\npattern=%s\ncache_start=%s\n"
           "read_bytes=8\nblock_size_bytes=%zu\ncache_payload_bytes=%zu\n"
           "working_set_bytes=%zu\nrequests=%" PRIu64 "\nhits=%" PRIu64
           "\nmisses=%" PRIu64 "\nevictions=%" PRIu64
           "\nload_errors=%" PRIu64 "\n",
           options.backend, options.pattern, options.b0 ? "warm_full" : "empty",
           options.block_size, options.cache_bytes, options.working_set_bytes,
           stats.requests, stats.hits, stats.misses, stats.evictions, stats.load_errors);
    printf("hit_rate=%.9f\nmiss_rate=%.9f\nchecksum=%" PRIu64 "\n",
           (double)stats.hits / (double)stats.requests,
           (double)stats.misses / (double)stats.requests, checksum);
    printf("seed=%" PRIu64 "\nprng=%s\nworkload_version=words8-v1\ntrace_hash=%016" PRIx64 "\n",
           options.seed, strcmp(options.pattern, "sequential") == 0 ? "none" : "splitmix64-v1",
           trace_hash);
    if (strcmp(options.pattern, "zipf") == 0)
        printf("zipf_theta=%.17g\nzipf_sampler=integral-envelope-v1\n", options.theta);
    else
        puts("zipf_theta=none\nzipf_sampler=none");
    printf("hot20_access_fraction=%.9f\n", (double)hot20_accesses / (double)stats.requests);
    printf("elapsed_seconds=%.9f\naccesses_per_second=%.3f\n",
           seconds, seconds > 0.0 ? (double)stats.requests / seconds : 0.0);
    printf("tcp_connections=%d\ntcp_requests=%" PRIu64 "\ntcp_payload_bytes=%" PRIu64 "\n",
           use_tcp ? 1 : 0, tcp.requests - warm_tcp_requests, tcp.payload_bytes - warm_tcp_bytes);
    printf("sd_open_direct=%d\nsd_alignment_source=%s\n"
           "sd_memory_alignment_bytes=%zu\nsd_offset_alignment_bytes=%zu\n"
           "sd_requests=%" PRIu64 "\nsd_read_calls=%" PRIu64
           "\nsd_read_bytes=%" PRIu64 "\n",
           use_sd ? 1 : 0, use_sd ? "statx" : "none",
           sd.memory_alignment, sd.offset_alignment,
           sd.requests - warm_sd_requests, sd.read_calls - warm_sd_calls, sd.read_bytes - warm_sd_bytes);
    puts("block_usage_window=measurement_only\nblock_usage_population=resident_at_start_or_loaded_during_measurement");
    printf("metrics_enabled=%d\n", options.metrics);
    puts("timing_scope=loop_including_generator_backend_checksums_and_optional_metrics_excluding_setup_and_summary");
    if (options.metrics && print_metrics(&latencies, cache, options.block_size) != 0)
        goto cleanup;
    exit_code = EXIT_SUCCESS;
cleanup:
    latency_store_destroy(&latencies);
    cache_destroy(cache);
    close_backends(&tcp, &sd);
    return exit_code;
}
