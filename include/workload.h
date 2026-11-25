#ifndef WORKLOAD_H
#define WORKLOAD_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t working_set_bytes;
    size_t next_address;
} sequential_workload_t;

int sequential_init(sequential_workload_t *workload, size_t working_set_bytes);
size_t sequential_next(sequential_workload_t *workload);

typedef enum { WORKLOAD_SEQUENTIAL, WORKLOAD_UNIFORM, WORKLOAD_ZIPF } workload_kind_t;

typedef struct {
    workload_kind_t kind;
    sequential_workload_t sequential;
    size_t items;
    uint64_t random_state;
    double theta;
    double envelope_area;
} workload_t;

/* Population consists of W/8 aligned logical words, independently of block
 * size, cache capacity and backend. Seed is a full uint64_t, including zero.
 * Zipf: 0 < theta <= 2, N <= 2^48. No allocation proportional to N.
 * Returns 0 on success or -1 for invalid configuration. */
int workload_init(workload_t *workload, const char *pattern,
                   size_t working_set_bytes, uint64_t seed, double theta);

/* Requires a successfully initialized workload; returns an aligned address
 * in [0, W-8]. Zipf uses libm; compare trace hashes when changing platforms. */
size_t workload_next(workload_t *workload);

#endif
