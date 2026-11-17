# Included from the existing Makefile; preserves all cache test targets.
CLIENT_SOURCES = src/cache.c src/workload.c src/client.c src/backend_tcp.c src/protocol.c
CLIENT_HEADERS = include/cache.h include/workload.h include/backend_tcp.h include/protocol.h
SERVER_SOURCES = src/server.c src/protocol.c

.PHONY: test-client sanitize-client clean-client test-tcp sanitize-tcp

all: build/client build/server
test: test-client test-tcp
sanitize: sanitize-client sanitize-tcp
clean: clean-client

build/client: $(CLIENT_SOURCES) $(CLIENT_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CLIENT_SOURCES) -o $@

test-client: build/client
	python3 tests/test_client.py ./build/client

build/client_sanitize: $(CLIENT_SOURCES) $(CLIENT_HEADERS) | build
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(CLIENT_SOURCES) -o $@

sanitize-client: build/client_sanitize
	python3 tests/test_client.py ./build/client_sanitize

clean-client:
	rm -f build/client build/client_sanitize build/server build/server_sanitize

build/server: $(SERVER_SOURCES) include/protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SERVER_SOURCES) -o $@

test-tcp: build/client build/server
	python3 tests/test_tcp.py ./build/client ./build/server

build/server_sanitize: $(SERVER_SOURCES) include/protocol.h | build
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(SERVER_SOURCES) -o $@

sanitize-tcp: build/client_sanitize build/server_sanitize
	python3 tests/test_tcp.py ./build/client_sanitize ./build/server_sanitize
