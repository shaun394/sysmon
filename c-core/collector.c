#include <stdio.h>
#include <windows.h>

static unsigned long long filetime_to_ull(FILETIME ft)
{
    return ((unsigned long long)ft.dwHighDateTime << 32) | (unsigned long long)ft.dwLowDateTime;
}

static double get_cpu_percent(void)
{
    FILETIME idle1, kernel1, user1;
    FILETIME idle2, kernel2, user2;

    if (!GetSystemTimes(&idle1, &kernel1, &user1))
    {
        return -1.0; // error
    }

    Sleep(200);

    if (!GetSystemTimes(&idle2, &kernel2, &user2))
    {
        return -1.0; // error
    }
    unsigned long long idleA = filetime_to_ull(idle1);
    unsigned long long kernelA = filetime_to_ull(kernel1);
    unsigned long long userA = filetime_to_ull(user1);

    unsigned long long idleB = filetime_to_ull(idle2);
    unsigned long long kernelB = filetime_to_ull(kernel2);
    unsigned long long userB = filetime_to_ull(user2);

    unsigned long long idleDelta = idleB - idleA;
    unsigned long long kernelDelta = kernelB - kernelA;
    unsigned long long userDelta = userB - userA;

    unsigned long long total = kernelDelta + userDelta;

    if (total == 0)
        return 0.0;

    double busy = (double)(total - idleDelta) * 100.0 / (double)total;
    return busy;
}

int main(void)
{
    /* --------- RAM --------- */
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

    double mem_used_percent = 0.0;
    if (total_mb > 0)
    {
        mem_used_percent = (double)used_mb * 100 / (double)total_mb;
    }

    /* --------- CPU --------- */
    double cpu_percent = get_cpu_percent();
    if (cpu_percent < 0.0)
    {
        printf("{\"ok\": false, \"error\": \"GetSystemTimes failed\"}\n");
        return 1;
    }

    /* --------- DISK (C:\) --------- */
    ULARGE_INTEGER freeBytesAvail, totalBytes, totalFreeBytes;

    if (!GetDiskFreeSpaceExA("C://", &freeBytesAvail, &totalBytes, &totalFreeBytes))
    {
        printf("{\"ok\": false, \"error\": \"GetDiskFreeSpaceExA failed\"}\n");
        return 1;
    }

    double disk_total_gb = (double)totalBytes.QuadPart / (1024 * 1024 * 1024);
    double disk_free_gb = (double)totalFreeBytes.QuadPart / (1024 * 1024 * 1024);
    double disk_used_gb = disk_total_gb - disk_free_gb;
    double disk_used_percent = 0.0;

    if (disk_used_percent > 0.0)
    {
        disk_used_percent = (disk_used_gb * 100) / disk_total_gb;
    }

    /* --------- OUTPUT JSON --------- */
    printf(
        "{\"ok\": true, \"cpu_percent\": %.1f, "
        "\"mem_total_mb\": %llu, \"mem_free_mb\": %llu, \"mem_used_mb\": %llu, \"mem_used_percent\": %.1f, "
        "\"disk_total_gb\": %.1f, \"disk_free_gb\": %.1f, \"disk_used_gb\": %.1f, \"disk_used_percent\": %.1f}\n",
        cpu_percent,
        total_mb, free_mb, used_mb, mem_used_percent,
        disk_total_gb, disk_free_gb, disk_used_gb, disk_used_percent);

    return 0;
}