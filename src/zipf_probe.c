#include "workload.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int unsigned_decimal(const char *text, uint64_t *value)
{
    if (*text == '\0')
        return -1;
    for (const char *p = text; *p; p++)
        if (*p < '0' || *p > '9')
            return -1;
    errno = 0;
    char *end = NULL;
    uintmax_t parsed = strtoumax(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed > UINT64_MAX)
        return -1;
    *value = (uint64_t)parsed;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fputs("Usage: zipf_probe WORKING_SET_BYTES THETA ACCESSES SEED\n", stderr);
        return EXIT_FAILURE;
    }
    uint64_t working_set, accesses, seed;
    if (unsigned_decimal(argv[1], &working_set) != 0 || working_set > SIZE_MAX ||
        unsigned_decimal(argv[3], &accesses) != 0 || accesses == 0 || accesses > 10000000 ||
        unsigned_decimal(argv[4], &seed) != 0) {
        fputs("Invalid probe integers (accesses must be 1..10000000).\n", stderr);
        return EXIT_FAILURE;
    }
    char *end = NULL;
    errno = 0;
    double theta = strtod(argv[2], &end);
    if (errno != 0 || end == argv[2] || *end != '\0' || !isfinite(theta) || theta <= 0 || theta > 2) {
        fputs("Invalid probe theta: require 0 < theta <= 2.\n", stderr);
        return EXIT_FAILURE;
    }
    workload_t workload;
    if (workload_init(&workload, "zipf", (size_t)working_set, seed, theta) != 0) {
        fputs("Invalid probe working set.\n", stderr);
        return EXIT_FAILURE;
    }
    size_t hot_items = workload.items / 5 + (workload.items % 5 != 0);
    uint64_t hot_accesses = 0, hash = UINT64_C(14695981039346656037);
    for (uint64_t i = 0; i < accesses; i++) {
        size_t address = workload_next(&workload);
        if (address / 8 < hot_items)
            hot_accesses++;
        hash = (hash ^ (uint64_t)address) * UINT64_C(1099511628211);
    }
    printf("{\"working_set_bytes\":%" PRIu64 ",\"theta\":%.17g,\"accesses\":%" PRIu64
           ",\"seed\":%" PRIu64 ",\"hot_items\":%zu,\"hot_accesses\":%" PRIu64
           ",\"hot_access_fraction\":%.12f,\"trace_hash\":\"%016" PRIx64 "\"}\n",
           working_set, theta, accesses, seed, hot_items, hot_accesses,
           (double)hot_accesses / (double)accesses, hash);
    return EXIT_SUCCESS;
}
