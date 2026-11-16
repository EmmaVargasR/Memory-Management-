CC = cc
CPPFLAGS = -Iinclude
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g

.PHONY: all test sanitize clean

all: build/test_cache

build:
	mkdir -p build

build/test_cache: src/cache.c tests/test_cache.c include/cache.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/cache.c tests/test_cache.c -o $@

test: build/test_cache
	./build/test_cache

build/test_cache_sanitize: src/cache.c tests/test_cache.c include/cache.h | build
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer src/cache.c tests/test_cache.c -o $@

sanitize: build/test_cache_sanitize
	./build/test_cache_sanitize

clean:
	rm -f build/test_cache build/test_cache_sanitize
