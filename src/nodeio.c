/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * client.c -- The phase 2 `inference` client (SPEC.md section 12).
 *
 * Minimal interactive IHM: colored banner, multi-line input in raw
 * termios mode, the frozen slash commands (/editor, /task, /submit,
 * /status, /quit), $EDITOR integration on a /tmp file, and a
 * narration line naming the POSIX node involved at each step.
 *
 * The client only ever talks to the mount point through plain POSIX
 * open/read/write: default /inference, overridable with --mount or
 * $INFERENCE_MOUNT (used by the tests against a fake tree).
 *
 * No TUI library: raw termios + ANSI codes, NO_COLOR honored, and the
 * interactive mode refuses to start without a terminal (exit 2).
 */

/* The interactive mode uses POSIX cbreak termios, NOT cfmakeraw():
 * cfmakeraw() clears OPOST (hence ONLCR), and then every '\n' printed
 * by the program moves down one line without returning to column 0 -
 * the staircase display.  With OPOST|ONLCR kept, a plain '\n' is
 * post-processed to CR LF as POSIX specifies. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "inference.h"
#include "client_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef VERSION
#define VERSION "0.1.0"
#endif
int
write_node_m (const char *mount, const char *node, const char *data,
              size_t len)
{
    char path[4096];
    if (node[0] == '\0')
        snprintf (path, sizeof path, "%s", mount);
    else
        snprintf (path, sizeof path, "%s/%s", mount, node);
    int fd = open (path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    size_t off = 0;
    while (off < len)
      {
        ssize_t n = write (fd, data + off, len - off);
        if (n <= 0)
          {
            close (fd);
            return -1;
          }
        off += (size_t) n;
      }
    if (close (fd) != 0)
        return -1;
    return 0;
}

/* write_node: same, on the /inference mount point. */
int
write_node (const char *node, const char *data, size_t len)
{
    return write_node_m (g_mount, node, data, len);
}

/* Same, on an explicit mount point (used for /page by /fetch). */
ssize_t
read_node_m (const char *mount, const char *node, char *out, size_t outsz)
{
    char path[4096];
    if (node[0] == '\0')
        snprintf (path, sizeof path, "%s", mount);
    else
        snprintf (path, sizeof path, "%s/%s", mount, node);
    int fd = open (path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t total = 0;
    for (;;)
      {
        if ((size_t) total >= outsz - 1)
          {
            /* keep reading to report the full length, discard */
            char sink[512];
            ssize_t n = read (fd, sink, sizeof sink);
            if (n < 0)
                break;
            if (n == 0)
                break;
            total += n;
            continue;
          }
        ssize_t n = read (fd, out + total, outsz - 1 - (size_t) total);
        if (n < 0)
            break;
        if (n == 0)
            break;
        total += n;
      }
    close (fd);
    if (outsz > 0)
        out[total < (ssize_t) outsz - 1 ? total : (ssize_t) outsz - 1] = '\0';
    return total;
}

/* Returns the content length, or -1 when unreadable (absent node,
 * ENODATA before any prompt...). */
ssize_t
read_node (const char *node, char *out, size_t outsz)
{
    return read_node_m (g_mount, node, out, outsz);
}

/* --- narration ------------------------------------------------------ */

void
narrate (const char *verb, const char *node)
{
    char line[4200];
    inf_client_narrate (line, sizeof line, verb, g_mount, node);
    if (inf_client_color ())
        printf ("\033[2m→ %s\033[0m\n", line);
    else
        printf ("→ %s\n", line);
}

/* --- commands ------------------------------------------------------- */

/* ANSI color for a state word (idle/running/parsed/done/error...),
 * empty string when plain output. */
const char *
state_ansi (const char *state)
{
    if (!inf_client_color ())
        return "";
    if (strcmp (state, "parsed") == 0 || strcmp (state, "done") == 0
        || strcmp (state, "ok") == 0)
        return "\033[32m";
    if (strcmp (state, "error") == 0 || strcmp (state, "failed") == 0)
        return "\033[1;31m";
    if (strcmp (state, "running") == 0 || strcmp (state, "pending") == 0)
        return "\033[33m";
    if (strcmp (state, "idle") == 0)
        return "\033[2m";
    return "";
}

void
print_status_node (void)
{
    char val[256];
    narrate ("read", INF_NODE_STATUS);
    if (read_node (INF_NODE_STATUS, val, sizeof val) < 0)
        printf ("  status illisible (translator non monté ?)\n");
    else
      {
        val[strcspn (val, "\r\n")] = '\0';
        const char *a = state_ansi (val);
        if (*a)
            printf ("  status: %s%s\033[0m\n", a, val);
        else
            printf ("  status: %s\n", val);
      }
}

void
cmd_status (void)
{
    print_status_node ();
}

/* narrate_orch — narration naming the /orchestrate mount point. */
void
narrate_orch (const char *verb, const char *node)
{
    char line[4200];
    inf_client_narrate (line, sizeof line, verb, g_orch, node);
    if (inf_client_color ())
        printf ("\033[2m→ %s\033[0m\n", line);
    else
        printf ("→ %s\n", line);
}

/* orch_read — read one role of /orchestrate.  Phase 2+ (netfs): plain
 * sub-nodes /orchestrate/status and /orchestrate/result.  Phase 1
 * (trivfs): a single shared slot serves every name, so a sub-node read
 * may succeed with the wrong role; inf_orch_role_ok detects that and we
 * fall back to the command protocol (write {"command":"<node>"} on the
 * mount root, read the root back).  The interface never writes a run
 * descriptor there (frozen contract). */
ssize_t
orch_read (const char *node, char *out, size_t outsz)
{
    ssize_t n = read_node_m (g_orch, node, out, outsz);
    if (n >= 0 && inf_orch_role_ok (node, out))
        return n;
    char cmd[64];
    snprintf (cmd, sizeof cmd, "{\"command\":\"%s\"}", node);
    if (write_node_m (g_orch, "", cmd, strlen (cmd)) != 0)
        return -1;
    return read_node_m (g_orch, "", out, outsz);
}
/* Write/read the root of a mount (node ""), e.g. the /db translator. */
int
write_root (const char *mount, const char *data, size_t len)
{
    char path[4096];
    snprintf (path, sizeof path, "%s", mount);
    int fd = open (path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    size_t off = 0;
    while (off < len)
      {
        ssize_t n = write (fd, data + off, len - off);
        if (n <= 0)
          {
            close (fd);
            return -1;
          }
        off += (size_t) n;
      }
    if (close (fd) != 0)
        return -1;
    return 0;
}

/* write_root_line — same, but appends the trailing '\n' the line
 * protocol of the /db translator requires: its io_write only processes
 * a line once the newline arrives, so a write without '\n' is silently
 * dropped and the next read returns the previous (stale) response. */
int
write_root_line (const char *mount, const char *data, size_t len)
{
    char *buf = malloc (len + 1);
    if (buf == NULL)
        return -1;
    memcpy (buf, data, len);
    buf[len] = '\n';
    int rc = write_root (mount, buf, len + 1);
    free (buf);
    return rc;
}

ssize_t
read_root (const char *mount, char *out, size_t outsz)
{
    return read_node_m (mount, "", out, outsz);
}
