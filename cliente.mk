# Included from the existing Makefile; preserves all cache test targets.
CLIENT_SOURCES = src/cache.c src/workload.c src/client.c
CLIENT_HEADERS = include/cache.h include/workload.h

.PHONY: test-client sanitize-client clean-client

all: build/client
test: test-client
sanitize: sanitize-client
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
	rm -f build/client build/client_sanitize
