#define _POSIX_C_SOURCE 200809L
#include "backend_tcp.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

static int connect_with_timeout(int fd, const struct sockaddr_in *address)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    int result = connect(fd, (const struct sockaddr *)address, sizeof(*address));
    if (result < 0) {
        if (errno != EINPROGRESS)
            return -1;
        struct pollfd event = {fd, POLLOUT, 0};
        do {
            result = poll(&event, 1, 5000);
        } while (result < 0 && errno == EINTR);
        if (result <= 0) {
            if (result == 0)
                errno = ETIMEDOUT;
            return -1;
        }
        int error = 0;
        socklen_t length = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0)
            return -1;
        if (error != 0) {
            errno = error;
            return -1;
        }
    }
    return fcntl(fd, F_SETFL, flags);
}

void tcp_backend_close(tcp_backend_t *backend)
{
    if (backend->fd >= 0) {
        close(backend->fd);
        backend->fd = -1;
    }
}

int tcp_backend_open(tcp_backend_t *backend, const char *host, uint16_t port,
                      size_t block_size, size_t working_set_bytes)
{
    *backend = (tcp_backend_t){.fd = -1, .block_size = block_size,
                               .working_set_bytes = working_set_bytes};
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1) {
        fputs("TCP host must be a numeric IPv4 address.\n", stderr);
        return -1;
    }
    backend->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (backend->fd < 0) {
        perror("TCP socket");
        return -1;
    }
    if (connect_with_timeout(backend->fd, &address) != 0 ||
        proto_configure_socket(backend->fd) != 0) {
        perror("TCP connection");
        tcp_backend_close(backend);
        return -1;
    }
    proto_header_t request = {PROTO_HELLO, PROTO_OK, block_size, working_set_bytes};
    proto_header_t reply;
    if (proto_send_header(backend->fd, request) != 0 ||
        proto_receive_header(backend->fd, &reply) != 1 ||
        reply.type != PROTO_HELLO_REPLY || reply.status != PROTO_OK ||
        reply.first != block_size || reply.second != working_set_bytes) {
        fputs("TCP handshake rejected or incomplete; check server sizes.\n", stderr);
        tcp_backend_close(backend);
        return -1;
    }
    return 0;
}

int tcp_backend_load(void *context, uint64_t block_id,
                      void *destination, size_t block_size)
{
    tcp_backend_t *backend = context;
    if (backend->fd < 0 || block_size != backend->block_size ||
        block_id >= backend->working_set_bytes / block_size)
        return -1;
    proto_header_t request = {PROTO_GET, PROTO_OK, block_id, 0};
    proto_header_t reply;
    backend->requests++;
    if (proto_send_header(backend->fd, request) != 0 ||
        proto_receive_header(backend->fd, &reply) != 1 ||
        reply.type != PROTO_GET_REPLY || reply.status != PROTO_OK ||
        reply.first != block_id || reply.second != block_size ||
        proto_receive_all(backend->fd, destination, block_size) != 1) {
        fputs("TCP block transfer failed or was incomplete.\n", stderr);
        tcp_backend_close(backend);
        return -1;
    }
    backend->payload_bytes += block_size;
    return 0;
}
