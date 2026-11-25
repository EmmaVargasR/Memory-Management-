# Included from the existing Makefile; preserves all cache test targets.
CLIENT_SOURCES = src/cache.c src/workload.c src/client.c src/backend_tcp.c src/protocol.c src/backend_sd.c
CLIENT_HEADERS = include/cache.h include/workload.h include/backend_tcp.h include/protocol.h include/backend_sd.h
SERVER_SOURCES = src/server.c src/protocol.c
SD_TEST_SOURCES = src/cache.c src/backend_sd.c tests/test_backend_sd.c
UBSAN_FLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
LDLIBS += -lm
WORKLOAD_TEST_SOURCES = src/workload.c tests/test_workload.c

.PHONY: test-client sanitize-client clean-client test-tcp sanitize-tcp test-sd ubsan test-sd-ubsan

all: build/client build/server
test: test-client test-tcp
sanitize: sanitize-client sanitize-tcp
clean: clean-client

build/client: $(CLIENT_SOURCES) $(CLIENT_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CLIENT_SOURCES) -o $@ $(LDLIBS)

test-client: build/client
	python3 tests/test_client.py ./build/client

build/client_sanitize: $(CLIENT_SOURCES) $(CLIENT_HEADERS) | build
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(CLIENT_SOURCES) -o $@ $(LDLIBS)

sanitize-client: build/client_sanitize
	python3 tests/test_client.py ./build/client_sanitize

clean-client:
	rm -f build/client build/client_sanitize build/server build/server_sanitize
	rm -f build/client_ubsan build/server_ubsan build/test_cache_ubsan build/test_backend_sd build/test_backend_sd_ubsan
	rm -f build/test_workload build/test_workload_ubsan build/test_workload_sanitize

build/server: $(SERVER_SOURCES) include/protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SERVER_SOURCES) -o $@

test-tcp: build/client build/server
	python3 tests/test_tcp.py ./build/client ./build/server

build/server_sanitize: $(SERVER_SOURCES) include/protocol.h | build
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(SERVER_SOURCES) -o $@

sanitize-tcp: build/client_sanitize build/server_sanitize
	python3 tests/test_tcp.py ./build/client_sanitize ./build/server_sanitize

# SD checks are explicit: they require regular files on a filesystem that
# reports direct-I/O alignment. They are not silently skipped on other hosts.
build/test_backend_sd: $(SD_TEST_SOURCES) include/backend_sd.h include/cache.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SD_TEST_SOURCES) -o $@

test-sd: build/client build/test_backend_sd
	python3 tests/test_sd.py ./build/client
	./build/test_backend_sd

build/client_ubsan: $(CLIENT_SOURCES) $(CLIENT_HEADERS) | build
	$(CC) $(CPPFLAGS) $(UBSAN_FLAGS) $(CLIENT_SOURCES) -o $@ $(LDLIBS)

build/server_ubsan: $(SERVER_SOURCES) include/protocol.h | build
	$(CC) $(CPPFLAGS) $(UBSAN_FLAGS) $(SERVER_SOURCES) -o $@

build/test_cache_ubsan: src/cache.c tests/test_cache.c include/cache.h | build
	$(CC) $(CPPFLAGS) $(UBSAN_FLAGS) src/cache.c tests/test_cache.c -o $@

ubsan: build/client_ubsan build/server_ubsan build/test_cache_ubsan
	./build/test_cache_ubsan
	python3 tests/test_client.py ./build/client_ubsan
	python3 tests/test_tcp.py ./build/client_ubsan ./build/server_ubsan

build/test_backend_sd_ubsan: $(SD_TEST_SOURCES) include/backend_sd.h include/cache.h | build
	$(CC) $(CPPFLAGS) $(UBSAN_FLAGS) $(SD_TEST_SOURCES) -o $@

test-sd-ubsan: build/client_ubsan build/test_backend_sd_ubsan
	python3 tests/test_sd.py ./build/client_ubsan
	./build/test_backend_sd_ubsan

.PHONY: test-workloads test-workloads-ubsan sanitize-workloads test-sd-patterns test-sd-patterns-ubsan
test: test-workloads
ubsan: test-workloads-ubsan
sanitize: sanitize-workloads
test-sd: test-sd-patterns
test-sd-ubsan: test-sd-patterns-ubsan

build/test_workload: $(WORKLOAD_TEST_SOURCES) include/workload.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WORKLOAD_TEST_SOURCES) -o $@ $(LDLIBS)

test-workloads: build/test_workload build/client build/server
	./build/test_workload
	python3 tests/test_patterns.py ./build/client ./build/server

build/test_workload_ubsan: $(WORKLOAD_TEST_SOURCES) include/workload.h | build
	$(CC) $(CPPFLAGS) $(UBSAN_FLAGS) $(WORKLOAD_TEST_SOURCES) -o $@ $(LDLIBS)

test-workloads-ubsan: build/test_workload_ubsan build/client_ubsan build/server_ubsan
	./build/test_workload_ubsan
	python3 tests/test_patterns.py ./build/client_ubsan ./build/server_ubsan

build/test_workload_sanitize: $(WORKLOAD_TEST_SOURCES) include/workload.h | build
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(WORKLOAD_TEST_SOURCES) -o $@ $(LDLIBS)

sanitize-workloads: build/test_workload_sanitize build/client_sanitize build/server_sanitize
	./build/test_workload_sanitize
	python3 tests/test_patterns.py ./build/client_sanitize ./build/server_sanitize

test-sd-patterns: build/client
	python3 tests/test_patterns.py ./build/client ./build/server sd

test-sd-patterns-ubsan: build/client_ubsan
	python3 tests/test_patterns.py ./build/client_ubsan ./build/server_ubsan sd
