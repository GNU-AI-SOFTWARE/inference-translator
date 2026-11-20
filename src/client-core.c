/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * client-core.c -- Pure, testable core of the phase 2 `inference`
 * client (SPEC.md section 12): editor resolution, slash command
 * parsing, narration formatting, NO_COLOR handling.  No TTY, no I/O:
 * the unit tests drive this directly.
 */

#include "inference.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char *
inf_client_editor (void)
{
    /* Frozen order (SPEC.md section 5): $VISUAL -> $EDITOR -> nano
     * -> vi.  The last two are probed in PATH so a missing nano falls
     * back to vi without a shell. */
    const char *env;
    if ((env = getenv ("VISUAL")) != NULL && *env != '\0')
        return env;
    if ((env = getenv ("EDITOR")) != NULL && *env != '\0')
        return env;

    static const char *const fallbacks[] = { "nano", "vi" };
    const char *path = getenv ("PATH");
    char buf[4096];
    if (path == NULL)
        path = "/usr/bin:/bin";
    for (size_t i = 0; i < sizeof fallbacks / sizeof fallbacks[0]; i++)
      {
        const char *p = path;
        while (*p != '\0')
          {
            const char *colon = strchr (p, ':');
            size_t dlen = colon != NULL ? (size_t) (colon - p) : strlen (p);
            if (dlen > 0 && dlen + 1 + strlen (fallbacks[i]) < sizeof buf)
              {
                memcpy (buf, p, dlen);
                buf[dlen] = '/';
                strcpy (buf + dlen + 1, fallbacks[i]);
                if (access (buf, X_OK) == 0)
                    return fallbacks[i];
              }
            if (colon == NULL)
                break;
            p = colon + 1;
          }
      }
    return "vi";
}

enum inf_client_cmd
inf_client_parse_command (const char *line, char *arg, size_t argsz)
{
    if (line == NULL || line[0] != '/')
        return INF_CMD_NONE;

    /* Split "command [argument]" on the first space. */
    const char *sp = strchr (line, ' ');
    size_t clen = sp != NULL ? (size_t) (sp - line) : strlen (line);

    for (size_t i = 0; i < inf_actions_count; i++)
        if (strlen (inf_actions[i].command) == clen
            && strncmp (line, inf_actions[i].command, clen) == 0)
          {
            enum inf_client_cmd cmd;
            if (strcmp (inf_actions[i].command, "/editor") == 0)
                cmd = INF_CMD_EDITOR;
            else if (strcmp (inf_actions[i].command, "/task") == 0)
                cmd = INF_CMD_TASK;
            else if (strcmp (inf_actions[i].command, "/submit") == 0)
                cmd = INF_CMD_SUBMIT;
            else if (strcmp (inf_actions[i].command, "/fetch") == 0)
                cmd = INF_CMD_FETCH;
            else if (strcmp (inf_actions[i].command, "/stats") == 0)
                cmd = INF_CMD_STATS;
            else if (strcmp (inf_actions[i].command, "/status") == 0)
                cmd = INF_CMD_STATUS;
            else if (strcmp (inf_actions[i].command, "/help") == 0)
                cmd = INF_CMD_HELP;
            else
                cmd = INF_CMD_QUIT;

            if ((cmd == INF_CMD_TASK || cmd == INF_CMD_FETCH)
                && arg != NULL && argsz > 0)
              {
                /* No space in the line ("/task" alone): sp is NULL, the
                   argument is empty - never evaluate sp + 1. */
                const char *a = sp != NULL ? sp + 1 : "";
                while (*a == ' ')
                    a++;
                size_t alen = strlen (a);
                while (alen > 0 && (a[alen - 1] == ' ' || a[alen - 1] == '\n'
                                    || a[alen - 1] == '\r'))
                    alen--;
                if (alen >= argsz)
                    alen = argsz - 1;
                memcpy (arg, a, alen);
                arg[alen] = '\0';
              }
            return cmd;
          }
    return INF_CMD_UNKNOWN;
}

size_t
inf_client_complete (const char *line, size_t len, char *out,
                     size_t outsz, const char **list, size_t list_max)
{
    const char *cands[16];
    size_t ncand = 0;
    size_t prefix = 0;
    const char *token;
    size_t toklen;

    if (len >= 6 && strncmp (line, "/task ", 6) == 0)
      {
        prefix = 6;
        token = line + 6;
        toklen = len - 6;
        for (int t = 0; t <= INF_TASK_CUSTOM; t++)
            cands[ncand++] = inf_task_name ((enum inf_task) t);
      }
    else if (len > 0 && line[0] == '/')
      {
        token = line;
        toklen = len;
        for (size_t i = 0; i < inf_actions_count && ncand < 16; i++)
            cands[ncand++] = inf_actions[i].command;
      }
    else
        return 0;

    size_t nmatch = 0;
    const char *single = NULL;
    for (size_t i = 0; i < ncand; i++)
      {
        if (strlen (cands[i]) >= toklen
            && strncmp (cands[i], token, toklen) == 0)
          {
            if (list != NULL && nmatch < list_max)
                list[nmatch] = cands[i];
            if (nmatch == 0)
                single = cands[i];
            nmatch++;
          }
      }
    if (nmatch == 1 && out != NULL && outsz > 0)
      {
        size_t n = prefix < outsz - 1 ? prefix : outsz - 1;
        memcpy (out, line, n);
        size_t rl = strlen (single);
        if (n + rl >= outsz)
            rl = outsz - 1 - n;
        memcpy (out + n, single, rl);
        out[n + rl] = '\0';
      }
    return nmatch;
}

bool
inf_client_color (void)
{
    const char *nc = getenv ("NO_COLOR");
    return !(nc != NULL && *nc != '\0');
}

size_t
inf_client_narrate (char *out, size_t outsz, const char *verb,
                    const char *mount, const char *node)
{
    int n = snprintf (out, outsz, "%s %s/%s", verb, mount, node);
    if (n < 0)
        return 0;
    return (size_t) n < outsz ? (size_t) n : outsz - 1;
}

/* --- phase 3: syntax highlighting (SPEC.md section 6) --------------- */

#define HL_PUT(str)                                                     \
    do {                                                                \
        size_t hl_l_ = strlen (str);                                    \
        if (n + hl_l_ < outsz)                                          \
          {                                                             \
            memcpy (out + n, str, hl_l_);                               \
            n += hl_l_;                                                 \
          }                                                             \
    } while (0)

/* Render JSON with ANSI colors when COLOR is true (keys bold cyan,
 * strings green, numbers magenta, literals yellow, punctuation dim);
 * with color=false the input is copied verbatim. */
size_t
inf_json_highlight (char *out, size_t outsz, const char *json, bool color)
{
    size_t n = 0;
    if (outsz == 0)
        return 0;
    if (!color)
      {
        size_t l = strlen (json);
        if (l > outsz - 1)
            l = outsz - 1;
        memcpy (out, json, l);
        out[l] = '\0';
        return l;
      }
    for (size_t i = 0; json[i] != '\0' && n + 16 < outsz;)
      {
        char c = json[i];
        if (c == '"')
          {
            /* scan the whole string token */
            size_t j = i + 1;
            while (json[j] != '\0' && json[j] != '"')
                j += (json[j] == '\\' && json[j + 1] != '\0') ? 2 : 1;
            if (json[j] == '"')
                j++;
            /* a string followed by ':' is an object key */
            size_t k = j;
            while (json[k] == ' ' || json[k] == '\t')
                k++;
            bool is_key = json[k] == ':';
            HL_PUT (is_key ? "\033[1;36m" : "\033[32m");
            if (n + (j - i) < outsz - 1)
              {
                memcpy (out + n, json + i, j - i);
                n += j - i;
              }
            HL_PUT ("\033[0m");
            i = j;
            continue;
          }
        if (c == '-' || (c >= '0' && c <= '9'))
          {
            size_t j = i;
            while (json[j] == '-' || json[j] == '+' || json[j] == '.'
                   || json[j] == 'e' || json[j] == 'E'
                   || (json[j] >= '0' && json[j] <= '9'))
                j++;
            HL_PUT ("\033[35m");
            if (n + (j - i) < outsz - 1)
              {
                memcpy (out + n, json + i, j - i);
                n += j - i;
              }
            HL_PUT ("\033[0m");
            i = j;
            continue;
          }
        if (strncmp (json + i, "true", 4) == 0
            || strncmp (json + i, "false", 5) == 0
            || strncmp (json + i, "null", 4) == 0)
          {
            size_t l = strncmp (json + i, "false", 5) == 0 ? 5 : 4;
            HL_PUT ("\033[33m");
            if (n + l < outsz - 1)
              {
                memcpy (out + n, json + i, l);
                n += l;
              }
            HL_PUT ("\033[0m");
            i += l;
            continue;
          }
        if (c == '{' || c == '}' || c == '[' || c == ']' || c == ':'
            || c == ',')
          {
            HL_PUT ("\033[2m");
            out[n++] = c;
            HL_PUT ("\033[0m");
            i++;
            continue;
          }
        out[n++] = c;
        i++;
      }
    out[n] = '\0';
    return n;
}

/* Render prompt text: http(s):// URLs underlined cyan, **bold**,
 * *italic* and `code` spans highlighted; plain copy when !color. */
size_t
inf_text_highlight (char *out, size_t outsz, const char *text, bool color)
{
    size_t n = 0;
    if (outsz == 0)
        return 0;
    if (!color)
      {
        size_t l = strlen (text);
        if (l > outsz - 1)
            l = outsz - 1;
        memcpy (out, text, l);
        out[l] = '\0';
        return l;
      }
    for (size_t i = 0; text[i] != '\0' && n + 16 < outsz;)
      {
        char c = text[i];
        if ((strncmp (text + i, "http://", 7) == 0
             || strncmp (text + i, "https://", 8) == 0))
          {
            size_t l = strncmp (text + i, "https://", 8) == 0 ? 8 : 7;
            size_t j = i + l;
            while (text[j] != '\0' && text[j] != ' ' && text[j] != '\t'
                   && text[j] != '\n')
                j++;
            HL_PUT ("\033[4;36m");
            if (n + (j - i) < outsz - 1)
              {
                memcpy (out + n, text + i, j - i);
                n += j - i;
              }
            HL_PUT ("\033[0m");
            i = j;
            continue;
          }
        if (strncmp (text + i, "**", 2) == 0)
          {
            size_t j = i + 2;
            while (text[j] != '\0' && strncmp (text + j, "**", 2) != 0)
                j++;
            if (text[j] == '*' && text[j + 1] == '*')
                j += 2;
            HL_PUT ("\033[1m");
            if (n + (j - i) < outsz - 1)
              {
                memcpy (out + n, text + i, j - i);
                n += j - i;
              }
            HL_PUT ("\033[0m");
            i = j;
            continue;
          }
        if (c == '*' || c == '`')
          {
            char close = c;
            size_t j = i + 1;
            while (text[j] != '\0' && text[j] != close)
                j++;
            if (text[j] == close)
                j++;
            HL_PUT (c == '*' ? "\033[3m" : "\033[2m");
            if (n + (j - i) < outsz - 1)
              {
                memcpy (out + n, text + i, j - i);
                n += j - i;
              }
            HL_PUT ("\033[0m");
            i = j;
            continue;
          }
        out[n++] = c;
        i++;
      }
    out[n] = '\0';
    return n;
}

#undef HL_PUT

size_t
inf_client_banner (char *out, size_t outsz, const char *version)
{
    if (inf_client_color ())
        return (size_t) snprintf (out, outsz,
                                  "\033[1;36minference\033[0m (GNU AI) %s "
                                  "- contrat v%d\n",
                                  version, INF_CONTRACT_VERSION);
    return (size_t) snprintf (out, outsz, "inference (GNU AI) %s - contrat v%d\n",
                              version, INF_CONTRACT_VERSION);
}

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
