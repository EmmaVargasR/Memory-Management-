#define _POSIX_C_SOURCE 200809L
#include "protocol.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

static void put32(unsigned char *bytes, uint32_t value)
{
    for (size_t i = 0; i < 4; i++)
        bytes[i] = (unsigned char)(value >> (24 - i * 8));
}

static void put64(unsigned char *bytes, uint64_t value)
{
    for (size_t i = 0; i < 8; i++)
        bytes[i] = (unsigned char)(value >> (56 - i * 8));
}

static uint32_t get32(const unsigned char *bytes)
{
    uint32_t value = 0;
    for (size_t i = 0; i < 4; i++)
        value = (value << 8) | bytes[i];
    return value;
}

static uint64_t get64(const unsigned char *bytes)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; i++)
        value = (value << 8) | bytes[i];
    return value;
}

int proto_send_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *bytes = buffer;
    size_t done = 0;
    while (done < length) {
        ssize_t sent = send(fd, bytes + done, length - done, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent <= 0) {
            if (sent == 0)
                errno = EPIPE;
            return -1;
        }
        done += (size_t)sent;
    }
    return 0;
}

int proto_receive_all(int fd, void *buffer, size_t length)
{
    unsigned char *bytes = buffer;
    size_t done = 0;
    while (done < length) {
        ssize_t received = recv(fd, bytes + done, length - done, 0);
        if (received < 0 && errno == EINTR)
            continue;
        if (received < 0)
            return -1;
        if (received == 0) {
            if (done == 0)
                return 0;
            errno = ECONNRESET;
            return -1;
        }
        done += (size_t)received;
    }
    return 1;
}

int proto_send_header(int fd, proto_header_t header)
{
    unsigned char bytes[PROTO_HEADER_BYTES];
    put32(bytes, PROTO_MAGIC);
    put32(bytes + 4, PROTO_VERSION);
    put32(bytes + 8, header.type);
    put32(bytes + 12, header.status);
    put64(bytes + 16, header.first);
    put64(bytes + 24, header.second);
    return proto_send_all(fd, bytes, sizeof(bytes));
}

int proto_receive_header(int fd, proto_header_t *header)
{
    unsigned char bytes[PROTO_HEADER_BYTES];
    int result = proto_receive_all(fd, bytes, sizeof(bytes));
    if (result != 1)
        return result;
    if (get32(bytes) != PROTO_MAGIC || get32(bytes + 4) != PROTO_VERSION) {
        errno = EPROTO;
        return -1;
    }
    header->type = get32(bytes + 8);
    header->status = get32(bytes + 12);
    header->first = get64(bytes + 16);
    header->second = get64(bytes + 24);
    return 1;
}

int proto_configure_socket(int fd)
{
    int enabled = 1;
    struct timeval timeout = {10, 0};
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0)
        return -1;
    return 0;
}
