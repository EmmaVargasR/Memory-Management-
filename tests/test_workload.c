#include "workload.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

static void test_sequential(void)
{
    workload_t workload;
    assert(workload_init(&workload, "sequential", 32, 42, 1.1) == 0);
    for (size_t i = 0; i < 100; i++)
        assert(workload_next(&workload) == (i % 4) * 8);
    puts("Sequential wraparound: OK");
}

static void test_reproducibility_and_bounds(void)
{
    const char *patterns[] = {"sequential", "uniform", "zipf"};
    const uint64_t seeds[] = {0, 42, UINT64_MAX};
    const double exponents[] = {0.01, 0.5, 1.0, 1.000000000001, 1.1, 2.0};
    for (size_t p = 0; p < 3; p++) {
        for (size_t s = 0; s < 3; s++) {
            for (size_t t = 0; t < 6; t++) {
                workload_t first, second;
                assert(workload_init(&first, patterns[p], 1048576, seeds[s], exponents[t]) == 0);
                assert(workload_init(&second, patterns[p], 1048576, seeds[s], exponents[t]) == 0);
                for (size_t i = 0; i < 1000; i++) {
                    size_t address = workload_next(&first);
                    assert(address == workload_next(&second));
                    assert(address < 1048576 && address % 8 == 0);
                }
            }
        }
        workload_t single;
        assert(workload_init(&single, patterns[p], 8, 42, 1.1) == 0);
        for (size_t i = 0; i < 100; i++)
            assert(workload_next(&single) == 0);
    }
    puts("Seeds, alignment, bounds and exponent edge cases: OK");
}

static void test_uniform_distribution(void)
{
    const size_t expected[] = {72, 8, 80, 24, 56, 56, 80, 96};
    workload_t golden;
    assert(workload_init(&golden, "uniform", 104, 42, 1.1) == 0);
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
        assert(workload_next(&golden) == expected[i]);
    size_t counts[10] = {0};
    workload_t workload;
    assert(workload_init(&workload, "uniform", 80, 42, 1.1) == 0);
    for (size_t i = 0; i < 100000; i++)
        counts[workload_next(&workload) / 8]++;
    for (size_t i = 0; i < 10; i++)
        assert(counts[i] > 9200 && counts[i] < 10800);
    puts("Uniform small-population frequencies: OK");
}

static void test_zipf_distribution(void)
{
    const double exponents[] = {0.5, 1.0, 1.1, 2.0};
    for (size_t t = 0; t < 4; t++) {
        size_t counts[10] = {0};
        double sum = 0.0;
        for (size_t k = 1; k <= 10; k++)
            sum += pow((double)k, -exponents[t]);
        workload_t workload;
        assert(workload_init(&workload, "zipf", 80, 42, exponents[t]) == 0);
        for (size_t i = 0; i < 200000; i++)
            counts[workload_next(&workload) / 8]++;
        for (size_t k = 1; k <= 10; k++) {
            double expected = pow((double)k, -exponents[t]) / sum;
            double observed = (double)counts[k - 1] / 200000.0;
            assert(fabs(observed - expected) < 0.006);
        }
    }
    puts("Zipf frequencies against normalized probability mass: OK");
}

static void test_invalid_configurations(void)
{
    workload_t workload;
    assert(workload_init(NULL, "uniform", 80, 1, 1.1) == -1);
    assert(workload_init(&workload, NULL, 80, 1, 1.1) == -1);
    assert(workload_init(&workload, "other", 80, 1, 1.1) == -1);
    assert(workload_init(&workload, "uniform", 0, 1, 1.1) == -1);
    assert(workload_init(&workload, "uniform", 17, 1, 1.1) == -1);
    assert(workload_init(&workload, "zipf", 80, 1, 0.0) == -1);
    assert(workload_init(&workload, "zipf", 80, 1, -1.0) == -1);
    assert(workload_init(&workload, "zipf", 80, 1, 2.1) == -1);
    assert(workload_init(&workload, "zipf", 80, 1, NAN) == -1);
    assert(workload_init(&workload, "zipf", 80, 1, INFINITY) == -1);
    puts("Invalid workload configurations: OK");
}

int main(void)
{
    test_sequential();
    test_reproducibility_and_bounds();
    test_uniform_distribution();
    test_zipf_distribution();
    test_invalid_configurations();
    puts("All workload tests passed");
    return 0;
}
