#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define PROTO_HEADER_BYTES 32
#define PROTO_MAGIC UINT32_C(0x4d4d4733)
#define PROTO_VERSION UINT32_C(1)
enum { PROTO_HELLO = 1, PROTO_GET = 2, PROTO_HELLO_REPLY = 3, PROTO_GET_REPLY = 4 };
enum { PROTO_OK = 0, PROTO_BAD_CONFIG = 1, PROTO_BAD_REQUEST = 2, PROTO_BAD_BLOCK = 3 };

typedef struct {
    uint32_t type, status;
    uint64_t first, second;
} proto_header_t;

/* Wire format: magic, version, type, status (4 bytes each), then two
 * 8-byte arguments. All fields are big endian; no native structs on wire.
 * HELLO: block size, working-set bytes. GET: block ID, zero.
 * Successful replies echo configuration, or block ID and payload bytes. */
int proto_send_all(int fd, const void *buffer, size_t length);
/* 1 = complete; 0 = EOF before any byte; -1 = error/truncated input. */
int proto_receive_all(int fd, void *buffer, size_t length);
int proto_send_header(int fd, proto_header_t header);
int proto_receive_header(int fd, proto_header_t *header);
/* TCP_NODELAY and 10-second socket send/receive timeouts. */
int proto_configure_socket(int fd);

#endif
