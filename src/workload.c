#include "workload.h"

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
