#ifndef BACKEND_SD_H
#define BACKEND_SD_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int fd;
    size_t block_size, working_set_bytes;
    size_t memory_alignment, offset_alignment;
    uint64_t requests, read_calls, read_bytes;
} sd_backend_t;

/* Linux regular file only. Opens read-only with O_DIRECT; requires reported
 * STATX_DIOALIGN constraints. Never falls back to buffered I/O. File length
 * must equal the requested working set. Call close even after load failure. */
int sd_backend_open(sd_backend_t *backend, const char *path,
                     size_t block_size, size_t working_set_bytes);
void sd_backend_close(sd_backend_t *backend);
int sd_backend_load(void *context, uint64_t block_id,
                     void *destination, size_t block_size);

#endif
