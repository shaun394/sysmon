// sysmon/c-core/collector.c
// Stream JSON system stats for a GUI (PySide6) at a target interval.
//
// Build (MSYS2 UCRT64):
//   cd /c/Users/Shaun/Desktop/Coding/sysmon/c-core
//   gcc collector.c -o collector.exe -lpdh -liphlpapi -lpsapi
//
// Run one-shot:
//   ./collector.exe
//
// Run stream @ 500ms:
//   ./collector.exe --stream 500

#define _WIN32_WINNT 0x0600

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <pdh.h>
#include <pdhmsg.h>

#include <iphlpapi.h>

#include <tlhelp32.h>
#include <psapi.h>

/* ============================================================
 * Global stop flag (Ctrl+C)
 * ============================================================ */

static volatile LONG g_running = 1;

static BOOL WINAPI console_ctrl_handler(DWORD ctrlType)
{
    switch (ctrlType)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        InterlockedExchange(&g_running, 0);
        return TRUE;
    default:
        return FALSE;
    }
}

/* ============================================================
 * Helpers
 * ============================================================ */

static unsigned long long filetime_to_ull(FILETIME ft)
{
    return ((unsigned long long)ft.dwHighDateTime << 32) | (unsigned long long)ft.dwLowDateTime;
}

/* Escape a process name for JSON (very small + safe: quotes/backslash/control) */
static void json_escape_into(const char *src, char *dst, size_t dst_cap)
{
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 2 < dst_cap; i++)
    {
        unsigned char c = (unsigned char)src[i];
        if (c == '\\' || c == '\"')
        {
            if (j + 2 >= dst_cap)
                break;
            dst[j++] = '\\';
            dst[j++] = (char)c;
        }
        else if (c == '\n')
        {
            if (j + 2 >= dst_cap)
                break;
            dst[j++] = '\\';
            dst[j++] = 'n';
        }
        else if (c == '\r')
        {
            if (j + 2 >= dst_cap)
                break;
            dst[j++] = '\\';
            dst[j++] = 'r';
        }
        else if (c == '\t')
        {
            if (j + 2 >= dst_cap)
                break;
            dst[j++] = '\\';
            dst[j++] = 't';
        }
        else if (c < 32)
        {
            // drop other control chars
        }
        else
        {
            dst[j++] = (char)c;
        }
    }
    dst[j] = '\0';
}

/* Take a snapshot of total bytes in/out across operational non-loopback interfaces */
static int get_total_net_bytes(unsigned long long *rx_bytes, unsigned long long *tx_bytes)
{
    if (!rx_bytes || !tx_bytes)
        return 0;

    DWORD size = 0;
    if (GetIfTable(NULL, &size, FALSE) != ERROR_INSUFFICIENT_BUFFER)
        return 0;

    MIB_IFTABLE *table = (MIB_IFTABLE *)malloc(size);
    if (!table)
        return 0;

    if (GetIfTable(table, &size, FALSE) != NO_ERROR)
    {
        free(table);
        return 0;
    }

    unsigned long long rx = 0, tx = 0;
    for (DWORD i = 0; i < table->dwNumEntries; i++)
    {
        MIB_IFROW *row = &table->table[i];

        if (row->dwType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        if (row->dwOperStatus != IF_OPER_STATUS_OPERATIONAL)
            continue;

        rx += (unsigned long long)row->dwInOctets;
        tx += (unsigned long long)row->dwOutOctets;
    }

    free(table);
    *rx_bytes = rx;
    *tx_bytes = tx;
    return 1;
}

/* ============================================================
 * PDH Disk Active Time (keep query open for speed)
 * ============================================================ */

typedef struct
{
    PDH_HQUERY query;
    PDH_HCOUNTER counter;
    int ok;
} DiskPdh;

static DiskPdh disk_pdh_init(void)
{
    DiskPdh d;
    d.query = NULL;
    d.counter = NULL;
    d.ok = 0;

    const char *path = "\\PhysicalDisk(_Total)\\% Disk Time";

    if (PdhOpenQueryA(NULL, 0, &d.query) != ERROR_SUCCESS)
        return d;
    if (PdhAddEnglishCounterA(d.query, path, 0, &d.counter) != ERROR_SUCCESS)
    {
        PdhCloseQuery(d.query);
        d.query = NULL;
        return d;
    }

    PdhCollectQueryData(d.query);
    d.ok = 1;
    return d;
}

static void disk_pdh_close(DiskPdh *d)
{
    if (!d)
        return;
    if (d->query)
    {
        PdhCloseQuery(d->query);
        d->query = NULL;
        d->counter = NULL;
    }
    d->ok = 0;
}

static double disk_pdh_read_percent(DiskPdh *d)
{
    if (!d || !d->ok)
        return -1.0;

    if (PdhCollectQueryData(d->query) != ERROR_SUCCESS)
        return -1.0;

    PDH_FMT_COUNTERVALUE value;
    DWORD type = 0;
    if (PdhGetFormattedCounterValue(d->counter, PDH_FMT_DOUBLE, &type, &value) != ERROR_SUCCESS)
    {
        return -1.0;
    }

    double v = value.doubleValue;
    if (v < 0.0)
        v = 0.0;
    if (v > 100.0)
        v = 100.0;
    return v;
}

/* ============================================================
 * CPU delta
 * ============================================================ */

typedef struct
{
    unsigned long long idle;
    unsigned long long kernel;
    unsigned long long user;
    int ok;
} CpuTimes;

static CpuTimes cpu_times_snapshot(void)
{
    CpuTimes t;
    t.idle = t.kernel = t.user = 0;
    t.ok = 0;

    FILETIME idleFt, kernelFt, userFt;
    if (!GetSystemTimes(&idleFt, &kernelFt, &userFt))
        return t;

    t.idle = filetime_to_ull(idleFt);
    t.kernel = filetime_to_ull(kernelFt);
    t.user = filetime_to_ull(userFt);
    t.ok = 1;
    return t;
}

static double cpu_percent_from_delta(CpuTimes a, CpuTimes b)
{
    if (!a.ok || !b.ok)
        return -1.0;

    unsigned long long idleDelta = (b.idle >= a.idle) ? (b.idle - a.idle) : 0;
    unsigned long long kernelDelta = (b.kernel >= a.kernel) ? (b.kernel - a.kernel) : 0;
    unsigned long long userDelta = (b.user >= a.user) ? (b.user - a.user) : 0;

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
 * Top processes by RAM (Working Set)
 * ============================================================ */

typedef struct
{
    char name[260];
    unsigned long long ram_mb;
} ProcEntry;

static int cmp_proc_desc(const void *a, const void *b)
{
    const ProcEntry *pa = (const ProcEntry *)a;
    const ProcEntry *pb = (const ProcEntry *)b;
    if (pb->ram_mb > pa->ram_mb)
        return 1;
    if (pb->ram_mb < pa->ram_mb)
        return -1;
    return 0;
}

static int get_top_processes_by_ram(ProcEntry *out, int out_cap, int top_n)
{
    if (!out || out_cap <= 0)
        return 0;
    if (top_n <= 0)
        top_n = 8;
    if (top_n > out_cap)
        top_n = out_cap;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32 pe;
    memset(&pe, 0, sizeof(pe));
    pe.dwSize = sizeof(pe);

    int count = 0;

    if (Process32First(snap, &pe))
    {
        do
        {
            // Open process for memory info (no admin in most cases)
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pe.th32ProcessID);
            if (!h)
                continue;

            PROCESS_MEMORY_COUNTERS pmc;
            if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc)))
            {
                unsigned long long ws = (unsigned long long)pmc.WorkingSetSize;
                unsigned long long mb = ws / (1024ULL * 1024ULL);

                if (mb > 0)
                {
                    ProcEntry e;
                    strncpy(e.name, pe.szExeFile, sizeof(e.name) - 1);
                    e.name[sizeof(e.name) - 1] = '\0';
                    e.ram_mb = mb;

                    if (count < out_cap)
                    {
                        out[count++] = e;
                    }
                }
            }

            CloseHandle(h);

        } while (Process32Next(snap, &pe));
    }

    CloseHandle(snap);

    if (count <= 0)
        return 0;

    qsort(out, count, sizeof(ProcEntry), cmp_proc_desc);
    if (count > top_n)
        count = top_n;

    return count;
}

/* ============================================================
 * Print JSON line
 * ============================================================ */

static int print_one_json(double cpu_percent,
                          double disk_active_percent,
                          double net_down_kbps,
                          double net_up_kbps,
                          const ProcEntry *procs,
                          int proc_count)
{

    // RAM
    MEMORYSTATUSEX mem;
    mem.dwLength = sizeof(mem);
    if (!GlobalMemoryStatusEx(&mem))
    {
        printf("{\"ok\": false, \"error\": \"GlobalMemoryStatusEx failed\"}\n");
        fflush(stdout);
        return 0;
    }

    unsigned long long total_mb = mem.ullTotalPhys / (1024ULL * 1024ULL);
    unsigned long long free_mb = mem.ullAvailPhys / (1024ULL * 1024ULL);
    unsigned long long used_mb = total_mb - free_mb;

    double mem_used_percent = 0.0;
    if (total_mb > 0)
        mem_used_percent = (double)used_mb * 100.0 / (double)total_mb;

    // Disk space
    ULARGE_INTEGER freeBytesAvail, totalBytes, totalFreeBytes;
    if (!GetDiskFreeSpaceExA("C:\\", &freeBytesAvail, &totalBytes, &totalFreeBytes))
    {
        printf("{\"ok\": false, \"error\": \"GetDiskFreeSpaceExA failed\"}\n");
        fflush(stdout);
        return 0;
    }

    double disk_total_gb = (double)totalBytes.QuadPart / (1024.0 * 1024.0 * 1024.0);
    double disk_free_gb = (double)totalFreeBytes.QuadPart / (1024.0 * 1024.0 * 1024.0);
    double disk_used_gb = disk_total_gb - disk_free_gb;

    // JSON start
    printf("{\"ok\": true, ");
    printf("\"cpu_percent\": %.1f, ", cpu_percent);

    printf("\"mem_total_mb\": %llu, \"mem_free_mb\": %llu, \"mem_used_mb\": %llu, \"mem_used_percent\": %.1f, ",
           total_mb, free_mb, used_mb, mem_used_percent);

    printf("\"disk_total_gb\": %.1f, \"disk_free_gb\": %.1f, \"disk_used_gb\": %.1f, ",
           disk_total_gb, disk_free_gb, disk_used_gb);

    printf("\"disk_active_percent\": %.1f, ", disk_active_percent);

    printf("\"net_down_kbps\": %.1f, \"net_up_kbps\": %.1f, ", net_down_kbps, net_up_kbps);

    // Top processes array
    printf("\"top_procs\": [");
    for (int i = 0; i < proc_count; i++)
    {
        char esc[600];
        json_escape_into(procs[i].name, esc, sizeof(esc));
        printf("{\"name\":\"%s\",\"ram_mb\":%llu", esc, procs[i].ram_mb);
        // CPU per-process comes next after this stage; keep field now for table compatibility:
        printf(",\"cpu_percent\":null}");
        if (i != proc_count - 1)
            printf(",");
    }
    printf("]}");
    printf("\n");

    fflush(stdout);
    return 1;
}

/* ============================================================
 * Modes
 * ============================================================ */

static int run_one_shot(void)
{
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    DiskPdh d = disk_pdh_init();
    if (!d.ok)
    {
        printf("{\"ok\": false, \"error\": \"PDH disk counter init failed\"}\n");
        fflush(stdout);
        return 1;
    }

    // CPU sample over ~200ms
    CpuTimes c1 = cpu_times_snapshot();
    unsigned long long rx1 = 0, tx1 = 0;
    get_total_net_bytes(&rx1, &tx1);

    Sleep(200);

    CpuTimes c2 = cpu_times_snapshot();
    unsigned long long rx2 = 0, tx2 = 0;
    get_total_net_bytes(&rx2, &tx2);

    double cpu = cpu_percent_from_delta(c1, c2);
    if (cpu < 0.0)
        cpu = 0.0;

    double disk_active = disk_pdh_read_percent(&d);
    if (disk_active < 0.0)
        disk_active = 0.0;

    // Network kbps from 0.2s window
    double seconds = 0.2;
    unsigned long long rx_delta = (rx2 >= rx1) ? (rx2 - rx1) : 0;
    unsigned long long tx_delta = (tx2 >= tx1) ? (tx2 - tx1) : 0;
    double down_kbps = ((double)rx_delta * 8.0) / 1000.0 / seconds;
    double up_kbps = ((double)tx_delta * 8.0) / 1000.0 / seconds;

    ProcEntry tmp[256];
    int proc_count = get_top_processes_by_ram(tmp, 256, 8);

    print_one_json(cpu, disk_active, down_kbps, up_kbps, tmp, proc_count);

    disk_pdh_close(&d);
    return 0;
}

static int run_stream(int interval_ms)
{
    if (interval_ms < 100)
        interval_ms = 100;
    if (interval_ms > 5000)
        interval_ms = 5000;

    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    DiskPdh d = disk_pdh_init();
    if (!d.ok)
    {
        printf("{\"ok\": false, \"error\": \"PDH disk counter init failed\"}\n");
        fflush(stdout);
        return 1;
    }

    CpuTimes prev_cpu = cpu_times_snapshot();
    unsigned long long prev_rx = 0, prev_tx = 0;
    get_total_net_bytes(&prev_rx, &prev_tx);

    ProcEntry cached[256];
    int cached_count = 0;

    // Update process list every 1000ms to keep 500ms stream smooth
    int proc_every_ms = 1000;
    int proc_tick = proc_every_ms;

    // Prime output quickly
    double disk0 = disk_pdh_read_percent(&d);
    if (disk0 < 0.0)
        disk0 = 0.0;

    cached_count = get_top_processes_by_ram(cached, 256, 8);
    print_one_json(0.0, disk0, 0.0, 0.0, cached, cached_count);

    while (InterlockedCompareExchange(&g_running, 1, 1) == 1)
    {
        Sleep((DWORD)interval_ms);

        CpuTimes now_cpu = cpu_times_snapshot();
        unsigned long long now_rx = 0, now_tx = 0;
        get_total_net_bytes(&now_rx, &now_tx);

        double cpu = cpu_percent_from_delta(prev_cpu, now_cpu);
        if (cpu < 0.0)
            cpu = 0.0;

        double disk_active = disk_pdh_read_percent(&d);
        if (disk_active < 0.0)
            disk_active = 0.0;

        double seconds = (double)interval_ms / 1000.0;
        unsigned long long rx_delta = (now_rx >= prev_rx) ? (now_rx - prev_rx) : 0;
        unsigned long long tx_delta = (now_tx >= prev_tx) ? (now_tx - prev_tx) : 0;

        double down_kbps = ((double)rx_delta * 8.0) / 1000.0 / seconds;
        double up_kbps = ((double)tx_delta * 8.0) / 1000.0 / seconds;
        if (down_kbps < 0.0)
            down_kbps = 0.0;
        if (up_kbps < 0.0)
            up_kbps = 0.0;

        proc_tick -= interval_ms;
        if (proc_tick <= 0)
        {
            cached_count = get_top_processes_by_ram(cached, 256, 8);
            proc_tick = proc_every_ms;
        }

        print_one_json(cpu, disk_active, down_kbps, up_kbps, cached, cached_count);

        prev_cpu = now_cpu;
        prev_rx = now_rx;
        prev_tx = now_tx;
    }

    disk_pdh_close(&d);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--stream") == 0)
    {
        int ms = 500;
        if (argc >= 3)
        {
            ms = atoi(argv[2]);
            if (ms <= 0)
                ms = 500;
        }
        return run_stream(ms);
    }
    return run_one_shot();
}
