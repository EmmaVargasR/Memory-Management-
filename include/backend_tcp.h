#ifndef BACKEND_TCP_H
#define BACKEND_TCP_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int fd;
    size_t block_size, working_set_bytes;
    uint64_t requests, payload_bytes;
} tcp_backend_t;

/* Numeric IPv4 address. One persistent connection; no silent retries. */
int tcp_backend_open(tcp_backend_t *backend, const char *host, uint16_t port,
                      size_t block_size, size_t working_set_bytes);
void tcp_backend_close(tcp_backend_t *backend);
int tcp_backend_load(void *context, uint64_t block_id,
                      void *destination, size_t block_size);

#endif
