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
/* --- Phase 6: session history (.history file, up/down arrows) ------- */

void
inf_history_init (struct inf_history *h)
{
    if (h == NULL)
        return;
    h->lines = NULL;
    h->count = 0;
    h->cap = 0;
}

void
inf_history_free (struct inf_history *h)
{
    if (h == NULL)
        return;
    for (size_t i = 0; i < h->count; i++)
        free (h->lines[i]);
    free (h->lines);
    h->lines = NULL;
    h->count = 0;
    h->cap = 0;
}

size_t
inf_history_add (struct inf_history *h, const char *line)
{
    if (h == NULL || line == NULL)
        return 0;
    size_t l = strlen (line);
    while (l > 0 && (line[l - 1] == ' ' || line[l - 1] == '\t'
                     || line[l - 1] == '\r' || line[l - 1] == '\n'))
        l--;
    if (l == 0)
        return h->count;
    /* HISTCONTROL=ignoredups: never append the same line twice in a row. */
    if (h->count > 0 && strncmp (h->lines[h->count - 1], line, l) == 0
        && h->lines[h->count - 1][l] == '\0')
        return h->count;
    if (h->count == h->cap)
      {
        size_t ncap = h->cap == 0 ? 32 : h->cap * 2;
        char **nl = realloc (h->lines, ncap * sizeof *nl);
        if (nl == NULL)
            return h->count;
        h->lines = nl;
        h->cap = ncap;
      }
    char *copy = malloc (l + 1);
    if (copy == NULL)
        return h->count;
    memcpy (copy, line, l);
    copy[l] = '\0';
    h->lines[h->count++] = copy;
    return h->count;
}

void
inf_history_load (struct inf_history *h, const char *path)
{
    if (h == NULL || path == NULL)
        return;
    FILE *f = fopen (path, "r");
    if (f == NULL)
        return;                         /* missing file is not an error */
    char *buf = NULL;
    size_t bufcap = 0, len = 0;
    int c;
    while ((c = fgetc (f)) != EOF)
      {
        if (len + 1 >= bufcap)
          {
            size_t ncap = bufcap == 0 ? 256 : bufcap * 2;
            char *nb = realloc (buf, ncap);
            if (nb == NULL)
                break;
            buf = nb;
            bufcap = ncap;
          }
        if (c == '\n')
          {
            buf[len] = '\0';
            inf_history_add (h, buf);   /* add() trims and dedupes */
            len = 0;
          }
        else
            buf[len++] = (char) c;
      }
    if (len > 0)                        /* last line without newline */
      {
        buf[len] = '\0';
        inf_history_add (h, buf);
      }
    free (buf);
    fclose (f);
}

const char *
inf_history_get (const struct inf_history *h, size_t i)
{
    if (h == NULL || i >= h->count)
        return NULL;
    return h->lines[i];
}

int
inf_history_append (const char *path, const char *line)
{
    if (path == NULL || line == NULL)
        return -1;
    int fd = open (path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0)
        return -1;
    size_t l = strlen (line);
    int rc = 0;
    if (write (fd, line, l) != (ssize_t) l
        || write (fd, "\n", 1) != 1)
        rc = -1;
    if (close (fd) != 0)
        rc = -1;
    return rc;
}

size_t
inf_format_uptime (char *out, size_t outsz, long seconds)
{
    if (out == NULL || outsz == 0)
        return 0;
    if (seconds < 0)
        seconds = 0;
    long d = seconds / 86400;
    long h = (seconds % 86400) / 3600;
    long m = (seconds % 3600) / 60;
    long sec = seconds % 60;
    int n;
    if (d > 0)
        n = snprintf (out, outsz, "%ldj %02ldh %02ldm %02lds", d, h, m, sec);
    else if (h > 0)
        n = snprintf (out, outsz, "%ldh %02ldm %02lds", h, m, sec);
    else if (m > 0)
        n = snprintf (out, outsz, "%ldm %02lds", m, sec);
    else
        n = snprintf (out, outsz, "%lds", sec);
    if (n < 0)
        n = 0;
    if ((size_t) n >= outsz)
        n = (int) outsz - 1;
    return (size_t) n;
}
