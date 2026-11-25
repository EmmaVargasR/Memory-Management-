#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "backend_sd.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

_Static_assert(sizeof(off_t) >= 8, "64-bit file offsets are required");

void sd_backend_close(sd_backend_t *backend)
{
    if (backend->fd >= 0) {
        close(backend->fd);
        backend->fd = -1;
    }
}

int sd_backend_open(sd_backend_t *backend, const char *path,
                     size_t block_size, size_t working_set_bytes)
{
    *backend = (sd_backend_t){.fd = -1, .block_size = block_size,
                              .working_set_bytes = working_set_bytes};
    if (path == NULL || block_size < 4096 || block_size % 4096 != 0 ||
        block_size > (size_t)SSIZE_MAX || working_set_bytes == 0 ||
        working_set_bytes % block_size != 0 || working_set_bytes > INT64_MAX) {
        fputs("SD invalid size: use blocks divisible by 4096 and valid 64-bit offsets.\n", stderr);
        return -1;
    }
    /* NONBLOCK avoids waiting if a special path is supplied accidentally.
     * The fstat check below only accepts regular files, never raw devices. */
    backend->fd = open(path, O_RDONLY | O_DIRECT | O_CLOEXEC | O_NONBLOCK);
    if (backend->fd < 0) {
        perror("SD open O_DIRECT");
        return -1;
    }
    struct stat information;
    if (fstat(backend->fd, &information) != 0) {
        perror("SD fstat");
        sd_backend_close(backend);
        return -1;
    }
    if (!S_ISREG(information.st_mode) || information.st_size < 0 ||
        (uintmax_t)information.st_size != (uintmax_t)working_set_bytes) {
        fputs("SD requires a regular file whose size exactly matches --working-set.\n", stderr);
        sd_backend_close(backend);
        return -1;
    }
#ifdef STATX_DIOALIGN
    struct statx direct = {0};
    if (statx(backend->fd, "", AT_EMPTY_PATH, STATX_DIOALIGN, &direct) != 0 ||
        !(direct.stx_mask & STATX_DIOALIGN) || direct.stx_dio_mem_align == 0 ||
        direct.stx_dio_offset_align == 0) {
        fputs("SD direct-I/O alignment unavailable from statx; buffered fallback is disabled.\n", stderr);
        sd_backend_close(backend);
        return -1;
    }
    backend->memory_alignment = direct.stx_dio_mem_align;
    backend->offset_alignment = direct.stx_dio_offset_align;
#else
    fputs("SD needs Linux headers with STATX_DIOALIGN support.\n", stderr);
    sd_backend_close(backend);
    return -1;
#endif
    if (block_size % backend->offset_alignment != 0) {
        fputs("SD block size does not meet the reported direct-I/O alignment.\n", stderr);
        sd_backend_close(backend);
        return -1;
    }
    return 0;
}

int sd_backend_load(void *context, uint64_t block_id,
                     void *destination, size_t block_size)
{
    sd_backend_t *backend = context;
    if (backend->fd < 0 || destination == NULL || block_size != backend->block_size ||
        block_size == 0 || backend->memory_alignment == 0 ||
        backend->offset_alignment == 0 ||
        block_id >= backend->working_set_bytes / block_size) {
        fputs("SD invalid block request.\n", stderr);
        return -1;
    }
    size_t offset = (size_t)block_id * block_size;
    if ((uintptr_t)destination % backend->memory_alignment != 0 ||
        offset % backend->offset_alignment != 0 ||
        block_size % backend->offset_alignment != 0) {
        fputs("SD buffer, offset or length violates direct-I/O alignment.\n", stderr);
        return -1;
    }
    backend->requests++;
    ssize_t received;
    do {
        backend->read_calls++;
        received = pread(backend->fd, destination, block_size, (off_t)offset);
    } while (received < 0 && errno == EINTR);
    if (received < 0) {
        perror("SD pread O_DIRECT");
        return -1;
    }
    if ((size_t)received != block_size) {
        fprintf(stderr, "SD short read: expected %zu bytes, received %zd.\n",
                block_size, received);
        return -1;
    }
    backend->read_bytes += block_size;
    return 0;
}
