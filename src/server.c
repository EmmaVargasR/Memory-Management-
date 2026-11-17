#define _POSIX_C_SOURCE 200809L
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int parse_size(const char *text, size_t *value)
{
    if (*text == '\0')
        return -1;
    for (const char *p = text; *p; p++)
        if (*p < '0' || *p > '9')
            return -1;
    errno = 0;
    char *end = NULL;
    uintmax_t number = strtoumax(text, &end, 10);
    if (errno != 0 || *end != '\0' || number > SIZE_MAX)
        return -1;
    *value = (size_t)number;
    return 0;
}

static void serve_client(int fd, const unsigned char *data,
                         size_t block_size, size_t working_set)
{
    if (proto_configure_socket(fd) != 0) {
        perror("configure client socket");
        return;
    }
    proto_header_t request;
    if (proto_receive_header(fd, &request) != 1)
        return;
    uint32_t status = PROTO_OK;
    if (request.type != PROTO_HELLO || request.status != PROTO_OK)
        status = PROTO_BAD_REQUEST;
    else if (request.first != block_size || request.second != working_set)
        status = PROTO_BAD_CONFIG;
    proto_header_t reply = {PROTO_HELLO_REPLY, status, block_size, working_set};
    if (proto_send_header(fd, reply) != 0 || status != PROTO_OK)
        return;

    uint64_t requests = 0, payload = 0;
    for (;;) {
        int result = proto_receive_header(fd, &request);
        if (result != 1)
            break;
        status = PROTO_OK;
        if (request.type != PROTO_GET || request.status != PROTO_OK || request.second != 0)
            status = PROTO_BAD_REQUEST;
        else if (request.first >= working_set / block_size)
            status = PROTO_BAD_BLOCK;
        reply = (proto_header_t){PROTO_GET_REPLY, status, request.first,
                                  status == PROTO_OK ? block_size : 0};
        if (proto_send_header(fd, reply) != 0 || status != PROTO_OK)
            break;
        size_t offset = (size_t)request.first * block_size;
        if (proto_send_all(fd, data + offset, block_size) != 0)
            break;
        requests++;
        payload += block_size;
    }
    printf("SESSION requests=%" PRIu64 " payload_bytes=%" PRIu64 "\n", requests, payload);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    const char *bind_address = "127.0.0.1";
    size_t port = 18083, block_size = 4096, working_set = 268435456;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 && argc == 2) {
            puts("Usage: server [--bind IPv4] [--port PORT] [--block-size BYTES] [--working-set BYTES]");
            return EXIT_SUCCESS;
        }
        if (i + 1 >= argc) {
            fputs("Missing server option value.\n", stderr);
            return EXIT_FAILURE;
        }
        if (strcmp(argv[i], "--bind") == 0) {
            bind_address = argv[++i];
            continue;
        }
        size_t *destination = NULL;
        if (strcmp(argv[i], "--port") == 0)
            destination = &port;
        else if (strcmp(argv[i], "--block-size") == 0)
            destination = &block_size;
        else if (strcmp(argv[i], "--working-set") == 0)
            destination = &working_set;
        if (destination == NULL || parse_size(argv[++i], destination) != 0) {
            fputs("Invalid server option or value.\n", stderr);
            return EXIT_FAILURE;
        }
    }
    if (port > 65535 || block_size < 8 || block_size % 8 != 0 ||
        working_set == 0 || working_set % block_size != 0) {
        fputs("Invalid port, block size or working set.\n", stderr);
        return EXIT_FAILURE;
    }
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    if (inet_pton(AF_INET, bind_address, &address.sin_addr) != 1) {
        fputs("Bind address must be numeric IPv4.\n", stderr);
        return EXIT_FAILURE;
    }
    unsigned char *data = malloc(working_set);
    if (data == NULL) {
        fputs("Could not allocate server working set.\n", stderr);
        return EXIT_FAILURE;
    }
    /* Touch the entire dataset before listening; GET never generates data. */
    for (size_t i = 0; i < working_set; i++)
        data[i] = (unsigned char)(i % 251);

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        perror("server socket");
        free(data);
        return EXIT_FAILURE;
    }
    int enabled = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0 ||
        bind(listener, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, 8) != 0) {
        perror("server listen");
        close(listener);
        free(data);
        return EXIT_FAILURE;
    }
    socklen_t address_size = sizeof(address);
    if (getsockname(listener, (struct sockaddr *)&address, &address_size) != 0) {
        perror("getsockname");
        close(listener);
        free(data);
        return EXIT_FAILURE;
    }
    printf("READY host=%s port=%u block_size=%zu working_set=%zu\n",
           bind_address, (unsigned int)ntohs(address.sin_port), block_size, working_set);
    fflush(stdout);
    /* One active client at a time; next clients are accepted after it closes. */
    for (;;) {
        int client = accept(listener, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR)
                continue;
            perror("server accept");
            break;
        }
        serve_client(client, data, block_size, working_set);
        close(client);
    }
    close(listener);
    free(data);
    return EXIT_FAILURE;
}
