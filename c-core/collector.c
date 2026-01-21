#include <stdio.h>
#include <windows.h>

int main(void)
{

    MEMORYSTATUSEX mem;
    mem.dwLength = sizeof(mem);

    if (GlobalMemoryStatusEx(&mem) == 0)
    {
        printf("{\"ok\": false, \"error\": \"GlobalMemoryStatusEx failed\"}\n");
        return 1;
    }

    unsigned long long total_mb = mem.ullTotalPhys / (1024ULL * 1024ULL);
    unsigned long long free_mb = mem.ullAvailPhys / (1024ULL * 1024ULL);
    unsigned long long used_mb = total_mb - free_mb;

    double used_percent = 0.0;
    if (total_mb > 0)
    {
        used_percent = (double)used_mb * 100 / (double)total_mb;
    }

    printf("{\"ok\": true, \"mem_total_mb\": %llu, \"mem_free_mb\": %llu, \"mem_used_mb\": %llu, \"mem_used_percent\": %.1f}\n", total_mb, free_mb, used_mb, used_percent);

    return 0;
}