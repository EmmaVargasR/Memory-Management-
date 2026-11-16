#ifndef WORKLOAD_H
#define WORKLOAD_H

#include <stddef.h>

typedef struct {
    size_t working_set_bytes;
    size_t next_address;
} sequential_workload_t;

/* Sequential 8-byte-aligned reads, wrapping at working_set_bytes. */
int sequential_init(sequential_workload_t *workload, size_t working_set_bytes);
size_t sequential_next(sequential_workload_t *workload);

#endif
