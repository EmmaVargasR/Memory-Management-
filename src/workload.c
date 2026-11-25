#include "workload.h"

#include <math.h>
#include <string.h>

int sequential_init(sequential_workload_t *workload, size_t working_set_bytes)
{
    if (workload == NULL || working_set_bytes < 8 || working_set_bytes % 8 != 0)
        return -1;
    workload->working_set_bytes = working_set_bytes;
    workload->next_address = 0;
    return 0;
}

size_t sequential_next(sequential_workload_t *workload)
{
    size_t address = workload->next_address;
    if (address == workload->working_set_bytes - 8)
        workload->next_address = 0;
    else
        workload->next_address += 8;
    return address;
}

static uint64_t random64(workload_t *workload)
{
    /* SplitMix64, with explicitly defined unsigned 64-bit arithmetic. */
    uint64_t value = (workload->random_state += UINT64_C(0x9e3779b97f4a7c15));
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static uint64_t uniform_bounded(workload_t *workload, uint64_t bound)
{
    /* Reject the incomplete modulo bucket to avoid modulo bias. */
    uint64_t threshold = (UINT64_C(0) - bound) % bound;
    uint64_t value;
    do {
        value = random64(workload);
    } while (value < threshold);
    return value % bound;
}

static double uniform_unit(workload_t *workload)
{
    return (double)(random64(workload) >> 11) * 0x1p-53;
}

static double power_integral(double x, double theta)
{
    double exponent = 1.0 - theta;
    if (exponent == 0.0)
        return log(x);
    return expm1(exponent * log(x)) / exponent;
}

static double inverse_integral(double area, double theta)
{
    double exponent = 1.0 - theta;
    if (exponent == 0.0)
        return exp(area);
    return exp(log1p(exponent * area) / exponent);
}

int workload_init(workload_t *workload, const char *pattern,
                   size_t working_set_bytes, uint64_t seed, double theta)
{
    if (workload == NULL || pattern == NULL || working_set_bytes < 8 ||
        working_set_bytes % 8 != 0)
        return -1;
    *workload = (workload_t){.items = working_set_bytes / 8,
                             .random_state = seed, .theta = theta};
    if (strcmp(pattern, "sequential") == 0) {
        workload->kind = WORKLOAD_SEQUENTIAL;
        return sequential_init(&workload->sequential, working_set_bytes);
    }
    if (strcmp(pattern, "uniform") == 0) {
        workload->kind = WORKLOAD_UNIFORM;
        return 0;
    }
    if (strcmp(pattern, "zipf") != 0 || !isfinite(theta) || theta <= 0.0 ||
        theta > 2.0 || (uint64_t)workload->items > (UINT64_C(1) << 48))
        return -1;
    workload->kind = WORKLOAD_ZIPF;
    workload->envelope_area = 1.0 + power_integral((double)workload->items, theta);
    if (!isfinite(workload->envelope_area) || workload->envelope_area < 1.0)
        return -1;
    return 0;
}

static size_t zipf_rank(workload_t *workload)
{
    if (workload->items == 1)
        return 0;
    /* Rejection envelope: mass 1 for rank 1, and density x^-theta over
     * [1,N] for the remaining ranks. On (k-1,k], x^-theta >= k^-theta.
     * Accepting with probability (x/k)^theta yields mass k^-theta.
     * Thus no CDF or rank table proportional to the working set is needed. */
    for (;;) {
        double area = uniform_unit(workload) * workload->envelope_area;
        if (area < 1.0)
            return 0;
        double x = inverse_integral(area - 1.0, workload->theta);
        if (!isfinite(x) || x < 1.0 || x > (double)workload->items)
            continue;
        size_t rank = (size_t)ceil(x);
        if (rank < 2)
            rank = 2;
        if (rank > workload->items)
            rank = workload->items;
        double acceptance = exp(workload->theta * (log(x) - log((double)rank)));
        if (uniform_unit(workload) < acceptance)
            return rank - 1;
    }
}

size_t workload_next(workload_t *workload)
{
    if (workload->kind == WORKLOAD_SEQUENTIAL)
        return sequential_next(&workload->sequential);
    if (workload->kind == WORKLOAD_UNIFORM)
        return (size_t)uniform_bounded(workload, (uint64_t)workload->items) * 8;
    return zipf_rank(workload) * 8;
}
