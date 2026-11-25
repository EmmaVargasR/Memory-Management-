#define _POSIX_C_SOURCE 200809L
#include "backend_sd.h"
#include "cache.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    char path[] = "build/sd-failure-XXXXXX";
    int file = mkstemp(path);
    assert(file >= 0);
    unsigned char data[8192];
    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = (unsigned char)(i % 251);
    size_t done = 0;
    while (done < sizeof(data)) {
        ssize_t written = write(file, data + done, sizeof(data) - done);
        if (written < 0 && errno == EINTR)
            continue;
        assert(written > 0);
        done += (size_t)written;
    }
    assert(fsync(file) == 0);

    sd_backend_t backend;
    assert(sd_backend_open(&backend, path, 4096, sizeof(data)) == 0);
    cache_t *cache = cache_create(1, 4096, sd_backend_load, &backend);
    assert(cache != NULL);
    unsigned char readout[8];
    assert(cache_read(cache, 0, 0, readout, sizeof(readout)) == 0);
    for (size_t i = 0; i < sizeof(readout); i++)
        assert(readout[i] == (unsigned char)i);

    /* Deliberate error injection after all previous I/O has completed. */
    assert(ftruncate(file, 0) == 0);
    assert(fsync(file) == 0);
    puts("Injecting EOF in a temporary file; one SD short-read diagnostic is expected.");
    fflush(stdout);
    assert(cache_read(cache, 1, 0, readout, sizeof(readout)) == -1);
    assert(cache_read(cache, 0, 0, readout, sizeof(readout)) == 0);
    for (size_t i = 0; i < sizeof(readout); i++)
        assert(readout[i] == (unsigned char)i);
    cache_stats_t stats = cache_get_stats(cache);
    assert(stats.requests == 3 && stats.hits == 1 && stats.misses == 2);
    assert(stats.load_errors == 1 && stats.evictions == 0);
    assert(backend.requests == 2 && backend.read_bytes == 4096);

    cache_destroy(cache);
    sd_backend_close(&backend);
    close(file);
    assert(unlink(path) == 0);
    puts("SD failure preserves cached data: OK");
    return 0;
}
