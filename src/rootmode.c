/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * rootmode.c -- the portable --root DIR engine, shared by both binary
 * faces (main.c on non-Hurd hosts, main-hurd.c on GNU/Hurd): without a
 * settrans mount, tests materialize the frozen /inference node tree as
 * plain files under DIR.  Pure POSIX, part of libinfcore.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "inference.h"

/* --- --root DIR: portable file-backed engine -----------------------
   On a non-Hurd host no translator can be mounted with settrans, so
   this mode materializes the frozen node tree as plain files under
   DIR: "prompt" and "task" are the writable inputs, every node is
   re-rendered from struct inf_fs whenever an input changes.  Used by
   tests/test_posix.sh to exercise the POSIX contract on any host. */

static volatile sig_atomic_t g_stop;

static void
on_stop_signal (int sig)
{
    (void) sig;
    g_stop = 1;
}

static ssize_t
read_file (const char *path, char *buf, size_t bufsz)
{
    int fd = open (path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t r = read (fd, buf, bufsz - 1);
    close (fd);
    if (r < 0)
        return -1;
    buf[r] = '\0';
    return r;
}

static int
write_file (const char *path, const char *data, size_t len)
{
    int fd = open (path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return -1;
    size_t off = 0;
    while (off < len)
      {
        ssize_t w = write (fd, data + off, len - off);
        if (w <= 0)
          {
            close (fd);
            return -1;
          }
        off += (size_t) w;
      }
    return close (fd) == 0 ? 0 : -1;
}

int
inf_rootmode_serve (const char *dir)
{
    char path[4096];
    char inbuf[INF_PROMPT_MAX + 1];
    char rbuf[INF_JSON_MAX * 4 + 1];
    char last_prompt[INF_PROMPT_MAX] = "";
    char last_task[256] = "";
    struct inf_fs fs;

    inf_fs_init (&fs);
    if (mkdir (dir, 0777) != 0 && errno != EEXIST)
      {
        fprintf (stderr, "inference-translator: --root %s: %s\n",
                 dir, strerror (errno));
        return EXIT_FAILURE;
      }
    for (size_t i = 0; i < inf_fs_node_count (); i++)
      {
        snprintf (path, sizeof path, "%s/%s", dir, inf_fs_nodes ()[i]);
        int fd = open (path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0)
          {
            fprintf (stderr, "inference-translator: %s: %s\n",
                     path, strerror (errno));
            return EXIT_FAILURE;
          }
        close (fd);
      }

    signal (SIGINT, on_stop_signal);
    signal (SIGTERM, on_stop_signal);
    signal (SIGHUP, on_stop_signal);

    while (!g_stop)
      {
        snprintf (path, sizeof path, "%s/%s", dir, INF_NODE_PROMPT);
        ssize_t r = read_file (path, inbuf, sizeof inbuf);
        if (r >= 0 && strcmp (inbuf, last_prompt) != 0)
          {
            (void) inf_fs_write (&fs, INF_NODE_PROMPT, inbuf, (size_t) r);
            size_t n = strlen (inbuf);
            if (n >= sizeof last_prompt)
                n = sizeof last_prompt - 1;
            memcpy (last_prompt, inbuf, n);
            last_prompt[n] = '\0';
          }
        snprintf (path, sizeof path, "%s/%s", dir, INF_NODE_TASK);
        r = read_file (path, inbuf, sizeof inbuf);
        if (r >= 0 && strcmp (inbuf, last_task) != 0)
          {
            (void) inf_fs_write (&fs, INF_NODE_TASK, inbuf, (size_t) r);
            size_t n = strlen (inbuf);
            if (n >= sizeof last_task)
                n = sizeof last_task - 1;
            memcpy (last_task, inbuf, n);
            last_task[n] = '\0';
          }
        for (size_t i = 0; i < inf_fs_node_count (); i++)
          {
            const char *node = inf_fs_nodes ()[i];
            ssize_t n = inf_fs_read (&fs, node, rbuf, sizeof rbuf - 1);
            if (n < 0)
                continue;
            rbuf[n] = '\0';
            snprintf (path, sizeof path, "%s/%s", dir, node);
            char cur[sizeof rbuf];
            ssize_t cr = read_file (path, cur, sizeof cur);
            if (cr != n || memcmp (cur, rbuf, (size_t) n) != 0)
                (void) write_file (path, rbuf, (size_t) n);
          }
        struct timespec ts = { 0, 50 * 1000 * 1000 };
        nanosleep (&ts, NULL);
      }
    return EXIT_SUCCESS;
}
