#define _POSIX_C_SOURCE 200809L
#include "cache.h"
#include "workload.h"
#include "backend_tcp.h"
#include "backend_sd.h"

#include <errno.h>
#include <inttypes.h>
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
} options_t;

static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
        "Usage: %s [--block-size BYTES] [--cache-size BYTES]\n"
        "          [--working-set BYTES] [--accesses COUNT]\n"
        "          [--backend synthetic|tcp|sd] [--host IPv4] [--port PORT]\n"
        "          [--file DATASET] (required only for --backend sd)\n"
        "Sizes are positive decimal bytes, without unit suffixes.\n"
        "Defaults: block=4096 cache=33554432 working-set=268435456 accesses=1000000\n"
        "Default backend=synthetic host=127.0.0.1 port=18083.\n"
        "Sequential 8-byte reads; starts with an empty cache.\n",
        program);
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
        if (strcmp(argv[i], "--backend") == 0 || strcmp(argv[i], "--host") == 0 ||
            strcmp(argv[i], "--file") == 0) {
            if (i + 1 >= argc) {
                fputs("Missing backend, host or file value.\n", stderr);
                return -1;
            }
            if (strcmp(argv[i], "--backend") == 0)
                options->backend = argv[++i];
            else if (strcmp(argv[i], "--host") == 0)
                options->host = argv[++i];
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

int main(int argc, char **argv)
{
    options_t options = {4096, 33554432, 268435456, 1000000,
                          "synthetic", "127.0.0.1", 18083, NULL};
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout, argv[0]);
        return EXIT_SUCCESS;
    }
    if (parse_options(argc, argv, &options) != 0) {
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }

    sequential_workload_t workload;
    if (sequential_init(&workload, options.working_set_bytes) != 0)
        return EXIT_FAILURE;
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
    cache_t *cache = cache_create(options.cache_bytes / options.block_size,
                                  options.block_size, loader, loader_context);
    if (cache == NULL) {
        fputs("Could not allocate cache.\n", stderr);
        close_backends(&tcp, &sd);
        return EXIT_FAILURE;
    }

    struct timespec start, end;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &start) != 0) {
        perror("clock_gettime");
        cache_destroy(cache);
        close_backends(&tcp, &sd);
        return EXIT_FAILURE;
    }
    uint64_t checksum = 0;
    for (size_t i = 0; i < options.accesses; i++) {
        size_t address = sequential_next(&workload);
        uint64_t block_id = (uint64_t)(address / options.block_size);
        size_t offset = address % options.block_size;
        unsigned char bytes[8];
        if (cache_read(cache, block_id, offset, bytes, sizeof(bytes)) != 0) {
            fprintf(stderr, "Read failed at access %zu.\n", i);
            cache_destroy(cache);
            close_backends(&tcp, &sd);
            return EXIT_FAILURE;
        }
        /* Defined unsigned wraparound; independent of machine byte order. */
        for (size_t j = 0; j < sizeof(bytes); j++)
            checksum += bytes[j];
    }
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &end) != 0) {
        perror("clock_gettime");
        cache_destroy(cache);
        close_backends(&tcp, &sd);
        return EXIT_FAILURE;
    }
    double seconds = elapsed_seconds(start, end);
    cache_stats_t stats = cache_get_stats(cache);
    printf("backend=%s\npattern=sequential\ncache_start=empty\n"
           "read_bytes=8\nblock_size_bytes=%zu\ncache_payload_bytes=%zu\n"
           "working_set_bytes=%zu\nrequests=%" PRIu64 "\nhits=%" PRIu64
           "\nmisses=%" PRIu64 "\nevictions=%" PRIu64
           "\nload_errors=%" PRIu64 "\n",
           options.backend, options.block_size, options.cache_bytes, options.working_set_bytes,
           stats.requests, stats.hits, stats.misses, stats.evictions, stats.load_errors);
    printf("hit_rate=%.9f\nmiss_rate=%.9f\nchecksum=%" PRIu64 "\n",
           (double)stats.hits / (double)stats.requests,
           (double)stats.misses / (double)stats.requests, checksum);
    printf("elapsed_seconds=%.9f\naccesses_per_second=%.3f\n",
           seconds, seconds > 0.0 ? (double)stats.requests / seconds : 0.0);
    printf("tcp_connections=%d\ntcp_requests=%" PRIu64 "\ntcp_payload_bytes=%" PRIu64 "\n",
           use_tcp ? 1 : 0, tcp.requests, tcp.payload_bytes);
    printf("sd_open_direct=%d\nsd_alignment_source=%s\n"
           "sd_memory_alignment_bytes=%zu\nsd_offset_alignment_bytes=%zu\n"
           "sd_requests=%" PRIu64 "\nsd_read_calls=%" PRIu64
           "\nsd_read_bytes=%" PRIu64 "\n",
           use_sd ? 1 : 0, use_sd ? "statx" : "none",
           sd.memory_alignment, sd.offset_alignment,
           sd.requests, sd.read_calls, sd.read_bytes);
    puts("timing_scope=loop_including_backend_loads_and_checksum_excluding_backend_setup");
    cache_destroy(cache);
    close_backends(&tcp, &sd);
    return EXIT_SUCCESS;
}
