/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * client-core.c -- Pure, testable core of the phase 2 `inference`
 * client (SPEC.md section 12): editor resolution, slash command
 * parsing, narration formatting, NO_COLOR handling.  No TTY, no I/O:
 * the unit tests drive this directly.
 */

#include "inference.h"

#include <fcntl.h>
#include <stdint.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* --- /stats: CPU / memory / I/O snapshot ----------------------------- */

#ifdef ON_HURD
#include <mach/port.h>
typedef mach_port_t *processor_name_array_t;   /* shim: identical redefinition */
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/host_info.h>
#include <mach/vm_statistics.h>
#endif

/* PUT - append one formatted chunk to the output block. */
#define PUT(...)                                                        \
    do {                                                                \
        int n = snprintf (p, left, __VA_ARGS__);                        \
        if (n < 0)                                                      \
            break;                                                      \
        if ((size_t) n >= left)                                         \
            n = (int) left - 1;                                         \
        p += n;                                                         \
        left -= (size_t) n;                                             \
    } while (0)

#ifdef ON_HURD

static void
stats_hurd (char *p, size_t left)
{
    static unsigned long prev_total, prev_idle;
    mach_port_t host = mach_host_self ();
    host_basic_info_data_t basic;
    mach_msg_type_number_t count = HOST_BASIC_INFO_COUNT;
    unsigned int ps = 0;

    cpu_load_info_data_t info;
    count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics (host, HOST_CPU_LOAD_INFO,
                         (host_info_t) &info, &count) == KERN_SUCCESS)
      {
        unsigned long total = 0, idle = info.cpu_ticks[CPU_STATE_IDLE];
        for (int i = 0; i < CPU_STATE_MAX; i++)
            total += info.cpu_ticks[i];
        if (prev_total != 0 && total > prev_total)
            PUT ("CPU  : %.1f %% (delta depuis le dernier /stats)\n",
                 100.0 * (double) ((total - prev_total) - (idle - prev_idle))
                 / (double) (total - prev_total));
        else
            PUT ("CPU  : ticks=%lu (premier appel)\n", total);
        prev_total = total;
        prev_idle = idle;
      }
    else
        PUT ("CPU  : n/d\n");

    if (host_page_size (host, &ps) == KERN_SUCCESS
        && host_info (host, HOST_BASIC_INFO, (host_info_t) &basic,
                      &count) == KERN_SUCCESS)
      {
        vm_statistics64_data_t vm;
        count = HOST_VM_INFO64_COUNT;
        unsigned long long total = basic.memory_size;
        unsigned long long used = total;
        if (host_statistics64 (host, HOST_VM_INFO64,
                               (host_info64_t) &vm, &count) == KERN_SUCCESS)
            used = total - (unsigned long long) vm.free_count * ps;
        PUT ("MEM  : %llu / %llu Mio (%.1f %%)\n",
             used >> 20, total >> 20, total ? 100.0 * (double) used / (double) total : 0.0);
      }
    else
        PUT ("MEM  : n/d\n");

    PUT ("IO   : n/d (pas de /proc sur Hurd)\n");
}

#else /* portable: /proc */

static double
proc_cpu_pct (void)
{
    static unsigned long long prev_total, prev_idle;
    FILE *f = fopen ("/proc/stat", "r");
    if (f == nullptr)
        return -1.0;
    unsigned long long v[10] = {0};
    int n = fscanf (f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                    &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                    &v[8], &v[9]);
    fclose (f);
    if (n < 4)
        return -1.0;
    unsigned long long total = 0, idle = v[3] + (n > 4 ? v[4] : 0);
    for (int i = 0; i < n; i++)
        total += v[i];
    double pct = -1.0;
    if (prev_total != 0 && total > prev_total)
        pct = 100.0 * (double) ((total - prev_total) - (idle - prev_idle))
              / (double) (total - prev_total);
    prev_total = total;
    prev_idle = idle;
    return pct;
}

static void
proc_mem (unsigned long long *used_kb, unsigned long long *total_kb)
{
    *used_kb = *total_kb = 0;
    FILE *f = fopen ("/proc/meminfo", "r");
    if (f == nullptr)
        return;
    unsigned long long total = 0, avail = 0;
    char key[64];
    unsigned long long val;
    while (fscanf (f, "%63s %llu", key, &val) == 2)
      {
        if (strcmp (key, "MemTotal:") == 0)
            total = val;
        else if (strcmp (key, "MemAvailable:") == 0)
            avail = val;
        fgets (key, sizeof key, f);   /* skip to next line */
      }
    fclose (f);
    *total_kb = total;
    *used_kb = total > avail ? total - avail : 0;
}

static void
proc_io (unsigned long long *disk_r, unsigned long long *disk_w,
         unsigned long long *net_rx, unsigned long long *net_tx)
{
    *disk_r = *disk_w = *net_rx = *net_tx = 0;
    FILE *f = fopen ("/proc/diskstats", "r");
    if (f != nullptr)
      {
        char line[512];
        while (fgets (line, sizeof line, f) != nullptr)
          {
            unsigned int maj, min;
            char name[64];
            unsigned long long rd, rd_sect, wr, wr_sect;
            if (sscanf (line, "%u %u %63s %llu %*u %llu %llu %*u %llu",
                        &maj, &min, name, &rd, &rd_sect, &wr, &wr_sect) == 7)
              {
                *disk_r += rd_sect * 512ULL;
                *disk_w += wr_sect * 512ULL;
              }
          }
        fclose (f);
      }
    f = fopen ("/proc/net/dev", "r");
    if (f != nullptr)
      {
        char line[512];
        while (fgets (line, sizeof line, f) != nullptr)
          {
            char *colon = strchr (line, ':');
            if (colon == nullptr)
                continue;
            char ifname[64];
            if (sscanf (line, " %63[^:]:", ifname) != 1)
                continue;
            if (strcmp (ifname, "lo") == 0)
                continue;
            unsigned long long rx = 0, tx = 0;
            if (sscanf (colon + 1,
                        " %llu %*u %*u %*u %*u %*u %*u %*u %llu",
                        &rx, &tx) == 2)
              {
                *net_rx += rx;
                *net_tx += tx;
              }
          }
        fclose (f);
      }
}

static void
stats_proc (char *p, size_t left)
{
    double cpu = proc_cpu_pct ();
    if (cpu >= 0.0)
        PUT ("CPU  : %.1f %%\n", cpu);
    else
        PUT ("CPU  : n/d (/proc/stat illisible)\n");

    unsigned long long used, total;
    proc_mem (&used, &total);
    if (total > 0)
        PUT ("MEM  : %llu / %llu Mio (%.1f %%)\n",
             used >> 10, total >> 10,
             100.0 * (double) used / (double) total);
    else
        PUT ("MEM  : n/d (/proc/meminfo illisible)\n");

    unsigned long long dr, dw, rx, tx;
    proc_io (&dr, &dw, &rx, &tx);
    PUT ("DISK : lus %llu Mio, écrits %llu Mio (depuis le boot)\n",
         dr >> 20, dw >> 20);
    PUT ("NET  : rx %llu Mio, tx %llu Mio (hors lo)\n", rx >> 20, tx >> 20);
}

#endif /* ON_HURD */

size_t
inf_client_stats (char *out, size_t outsz)
{
    if (out == NULL || outsz == 0)
        return 0;
    char *p = out;
    size_t left = outsz;
#ifdef ON_HURD
    stats_hurd (p, left);
#else
    stats_proc (p, left);
#endif
    out[outsz - 1] = '\0';
    return (size_t) (p - out);
}

/* inf_client_stats_line: compact one-line snapshot for the persistent
 * info line above the prompt: "CPU 12.3 % | MEM 456/2048 Mio (22.3 %)".
 * Same sources as inf_client_stats, single line, bounded. */
size_t
inf_client_stats_line (char *out, size_t outsz)
{
    if (out == NULL || outsz == 0)
        return 0;
#ifdef ON_HURD
    {
        static unsigned long prev_total, prev_idle;
        mach_port_t host = mach_host_self ();
        cpu_load_info_data_t info;
        mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
        double cpu = -1.0;
        if (host_statistics (host, HOST_CPU_LOAD_INFO,
                             (host_info_t) &info, &count) == KERN_SUCCESS)
          {
            unsigned long total = 0, idle = info.cpu_ticks[CPU_STATE_IDLE];
            for (int i = 0; i < CPU_STATE_MAX; i++)
                total += info.cpu_ticks[i];
            if (prev_total != 0 && total > prev_total)
                cpu = 100.0 * (double) ((total - prev_total)
                                         - (idle - prev_idle))
                      / (double) (total - prev_total);
            prev_total = total;
            prev_idle = idle;
          }
        host_basic_info_data_t basic;
        unsigned int ps = 0;
        unsigned long long used = 0, totalm = 0;
        if (host_page_size (host, &ps) == KERN_SUCCESS
            && host_info (host, HOST_BASIC_INFO, (host_info_t) &basic,
                          &count) == KERN_SUCCESS)
          {
            totalm = basic.memory_size;
            used = totalm;
            vm_statistics64_data_t vm;
            count = HOST_VM_INFO64_COUNT;
            if (host_statistics64 (host, HOST_VM_INFO64,
                                   (host_info64_t) &vm, &count)
                == KERN_SUCCESS)
                used = totalm - (unsigned long long) vm.free_count * ps;
          }
        int n;
        if (cpu >= 0.0 && totalm > 0)
            n = snprintf (out, outsz, "CPU %.1f %% | MEM %llu/%llu Mio (%.1f %%)",
                          cpu, used >> 20, totalm >> 20,
                          100.0 * (double) used / (double) totalm);
        else if (totalm > 0)
            n = snprintf (out, outsz, "CPU n/d | MEM %llu/%llu Mio (%.1f %%)",
                          used >> 20, totalm >> 20,
                          100.0 * (double) used / (double) totalm);
        else
            n = snprintf (out, outsz, "CPU n/d | MEM n/d");
        return n < 0 ? 0 : (size_t) n;
    }
#else
    {
        double cpu = proc_cpu_pct ();
        unsigned long long used, total;
        proc_mem (&used, &total);
        int n;
        if (cpu >= 0.0 && total > 0)
            n = snprintf (out, outsz,
                          "CPU %.1f %% | MEM %llu/%llu Mio (%.1f %%)",
                          cpu, used >> 10, total >> 10,
                          100.0 * (double) used / (double) total);
        else if (total > 0)
            n = snprintf (out, outsz, "CPU n/d | MEM %llu/%llu Mio (%.1f %%)",
                          used >> 10, total >> 10,
                          100.0 * (double) used / (double) total);
        else if (cpu >= 0.0)
            n = snprintf (out, outsz, "CPU %.1f %% | MEM n/d", cpu);
        else
            n = snprintf (out, outsz, "CPU n/d | MEM n/d");
        return n < 0 ? 0 : (size_t) n;
    }
#endif
}
