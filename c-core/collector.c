// sysmon/c-core/collector.c
// Builds a JSON “snapshot” of system stats for the Python dashboard.
//
// Compile (MSYS2 UCRT64):
//   cd /c/Users/Shaun/Desktop/Coding/sysmon/c-core
//   gcc collector.c -o collector.exe -lpdh -liphlpapi

#define _WIN32_WINNT 0x0600 // Enable newer Windows networking APIs (Vista+)

#include <stdio.h>
#include <windows.h>

#include <pdh.h>
#include <pdhmsg.h>

#include <iphlpapi.h>
#include <netioapi.h>
#include <stdlib.h>

/* ============================================================
 * CPU
 * ============================================================ */

static unsigned long long filetime_to_ull(FILETIME ft)
{
    return ((unsigned long long)ft.dwHighDateTime << 32) | (unsigned long long)ft.dwLowDateTime;
}

static double get_cpu_percent(void)
{
    FILETIME idle1, kernel1, user1;
    FILETIME idle2, kernel2, user2;

    if (!GetSystemTimes(&idle1, &kernel1, &user1))
        return -1.0;

    Sleep(200);

    if (!GetSystemTimes(&idle2, &kernel2, &user2))
        return -1.0;

    unsigned long long idleA = filetime_to_ull(idle1);
    unsigned long long kernelA = filetime_to_ull(kernel1);
    unsigned long long userA = filetime_to_ull(user1);

    unsigned long long idleB = filetime_to_ull(idle2);
    unsigned long long kernelB = filetime_to_ull(kernel2);
    unsigned long long userB = filetime_to_ull(user2);

    unsigned long long idleDelta = idleB - idleA;
    unsigned long long kernelDelta = kernelB - kernelA;
    unsigned long long userDelta = userB - userA;

    // Kernel time includes idle time, so total = kernel + user
    unsigned long long total = kernelDelta + userDelta;
    if (total == 0)
        return 0.0;

    double busy = (double)(total - idleDelta) * 100.0 / (double)total;
    if (busy < 0.0)
        busy = 0.0;
    if (busy > 100.0)
        busy = 100.0;

    return busy;
}

/* ============================================================
 * DISK ACTIVE TIME (Task Manager-style) via PDH
 * ============================================================ */

static double get_disk_active_percent(void)
{
    PDH_HQUERY query = NULL;
    PDH_HCOUNTER counter = NULL;

    const char *path = "\\PhysicalDisk(_Total)\\% Disk Time";

    if (PdhOpenQueryA(NULL, 0, &query) != ERROR_SUCCESS)
        return -1.0;

    if (PdhAddEnglishCounterA(query, path, 0, &counter) != ERROR_SUCCESS)
    {
        PdhCloseQuery(query);
        return -1.0;
    }

    // Prime + sample
    PdhCollectQueryData(query);
    Sleep(200);
    PdhCollectQueryData(query);

    PDH_FMT_COUNTERVALUE value;
    DWORD type = 0;

    if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, &type, &value) != ERROR_SUCCESS)
    {
        PdhCloseQuery(query);
        return -1.0;
    }

    PdhCloseQuery(query);

    double v = value.doubleValue;
    if (v < 0.0)
        v = 0.0;
    if (v > 100.0)
        v = 100.0;
    return v;
}

/* ============================================================
 * NETWORK SPEED (kbps) via IP Helper (GetIfTable2)
 * ============================================================ */

static int get_network_speeds_kbps(double *down_kbps, double *up_kbps)
{
    if (!down_kbps || !up_kbps)
        return 0;

    DWORD size = 0;

    // First call: ask Windows how big the table buffer must be
    if (GetIfTable(NULL, &size, FALSE) != ERROR_INSUFFICIENT_BUFFER)
        return 0;

    MIB_IFTABLE *t1 = (MIB_IFTABLE *)malloc(size);
    if (!t1)
        return 0;

    if (GetIfTable(t1, &size, FALSE) != NO_ERROR)
    {
        free(t1);
        return 0;
    }

    unsigned long long rx1 = 0, tx1 = 0;
    for (DWORD i = 0; i < t1->dwNumEntries; i++)
    {
        MIB_IFROW *row = &t1->table[i];

        // Skip loopback and interfaces that are down
        if (row->dwType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        if (row->dwOperStatus != IF_OPER_STATUS_OPERATIONAL)
            continue;

        rx1 += row->dwInOctets;
        tx1 += row->dwOutOctets;
    }

    free(t1);

    Sleep(1000);

    size = 0;
    if (GetIfTable(NULL, &size, FALSE) != ERROR_INSUFFICIENT_BUFFER)
        return 0;

    MIB_IFTABLE *t2 = (MIB_IFTABLE *)malloc(size);
    if (!t2)
        return 0;

    if (GetIfTable(t2, &size, FALSE) != NO_ERROR)
    {
        free(t2);
        return 0;
    }

    unsigned long long rx2 = 0, tx2 = 0;
    for (DWORD i = 0; i < t2->dwNumEntries; i++)
    {
        MIB_IFROW *row = &t2->table[i];

        if (row->dwType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        if (row->dwOperStatus != IF_OPER_STATUS_OPERATIONAL)
            continue;

        rx2 += row->dwInOctets;
        tx2 += row->dwOutOctets;
    }

    free(t2);

    unsigned long long rx_delta = (rx2 >= rx1) ? (rx2 - rx1) : 0;
    unsigned long long tx_delta = (tx2 >= tx1) ? (tx2 - tx1) : 0;

    // bytes/sec -> kilobits/sec
    *down_kbps = (double)(rx_delta * 8ULL) / 1000.0;
    *up_kbps = (double)(tx_delta * 8ULL) / 1000.0;

    if (*down_kbps < 0.0)
        *down_kbps = 0.0;
    if (*up_kbps < 0.0)
        *up_kbps = 0.0;

    return 1;
}

/* ============================================================
 * MAIN
 * ============================================================ */

int main(void)
{
    /* --------- RAM --------- */
    MEMORYSTATUSEX mem;
    mem.dwLength = sizeof(mem);

    if (!GlobalMemoryStatusEx(&mem))
    {
        printf("{\"ok\": false, \"error\": \"GlobalMemoryStatusEx failed\"}\n");
        return 1;
    }

    unsigned long long total_mb = mem.ullTotalPhys / (1024ULL * 1024ULL);
    unsigned long long free_mb = mem.ullAvailPhys / (1024ULL * 1024ULL);
    unsigned long long used_mb = total_mb - free_mb;

    double mem_used_percent = 0.0;
    if (total_mb > 0)
        mem_used_percent = (double)used_mb * 100.0 / (double)total_mb;

    /* --------- CPU --------- */
    double cpu_percent = get_cpu_percent();
    if (cpu_percent < 0.0)
    {
        printf("{\"ok\": false, \"error\": \"GetSystemTimes failed\"}\n");
        return 1;
    }

    /* --------- DISK SPACE (still useful as text) --------- */
    ULARGE_INTEGER freeBytesAvail, totalBytes, totalFreeBytes;

    if (!GetDiskFreeSpaceExA("C:\\", &freeBytesAvail, &totalBytes, &totalFreeBytes))
    {
        printf("{\"ok\": false, \"error\": \"GetDiskFreeSpaceExA failed\"}\n");
        return 1;
    }

    double disk_total_gb = (double)totalBytes.QuadPart / (1024.0 * 1024.0 * 1024.0);
    double disk_free_gb = (double)totalFreeBytes.QuadPart / (1024.0 * 1024.0 * 1024.0);
    double disk_used_gb = disk_total_gb - disk_free_gb;

    /* --------- DISK ACTIVE TIME (Task Manager-style) --------- */
    double disk_active_percent = get_disk_active_percent();
    if (disk_active_percent < 0.0)
    {
        printf("{\"ok\": false, \"error\": \"PDH disk counter failed\"}\n");
        return 1;
    }

    /* --------- NETWORK --------- */
    double net_down_kbps = 0.0, net_up_kbps = 0.0;
    if (!get_network_speeds_kbps(&net_down_kbps, &net_up_kbps))
    {
        printf("{\"ok\": false, \"error\": \"Network counters failed\"}\n");
        return 1;
    }

    /* --------- OUTPUT JSON --------- */
    printf(
        "{\"ok\": true, "
        "\"cpu_percent\": %.1f, "
        "\"mem_total_mb\": %llu, \"mem_free_mb\": %llu, \"mem_used_mb\": %llu, \"mem_used_percent\": %.1f, "
        "\"disk_total_gb\": %.1f, \"disk_free_gb\": %.1f, \"disk_used_gb\": %.1f, "
        "\"disk_active_percent\": %.1f, "
        "\"net_down_kbps\": %.1f, \"net_up_kbps\": %.1f}\n",
        cpu_percent,
        total_mb, free_mb, used_mb, mem_used_percent,
        disk_total_gb, disk_free_gb, disk_used_gb,
        disk_active_percent,
        net_down_kbps, net_up_kbps);

    return 0;
}
