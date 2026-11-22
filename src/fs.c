/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * fs.c -- The /inference translator engine (phase 1 MVP), pure
 * C23/POSIX.
 *
 * This implements the frozen tree of SPEC.md section 2 as plain node
 * semantics: the GNU/Hurd netfs glue (netfs-hooks.c) and the unit
 * tests (tests/test_fs.c) both drive it, so the whole contract is
 * testable on any POSIX system, Hurd or not.
 *
 * Semantics:
 *   - write "prompt": store the raw prompt, parse it into the type 1
 *     request (contract.c), status becomes "parsed".  The task is
 *     re-detected unless /inference/task was manually overridden.
 *   - write "task": override the task (sticky until changed again);
 *     the last request is updated too.  Writing "auto" clears the
 *     override: the task is detected again from the current prompt.
 *   - read "request"/"urls" before any prompt: -1 (nothing parsed yet).
 *   - read "params": always "{}" in phase 1.
 *   - read "version": {"program":...,"version":...,"contract":1}.
 *
 * No allocation in the hot paths: everything lives in the caller's
 * struct inf_fs, and reads render into a caller-provided buffer.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifndef VERSION
# ifdef PACKAGE_VERSION
#  define VERSION PACKAGE_VERSION
# else
#  define VERSION "0.1.0"
# endif
#endif

#include <stdio.h>
#include <string.h>

#include "inference.h"

static const char *const node_names[] =
{
    INF_NODE_PROMPT, INF_NODE_REQUEST, INF_NODE_URLS, INF_NODE_TASK,
    INF_NODE_PARAMS, INF_NODE_STATUS, INF_NODE_VERSION,
};

const char *const *
inf_fs_nodes (void)
{
    return node_names;
}

size_t
inf_fs_node_count (void)
{
    return sizeof node_names / sizeof node_names[0];
}

static int
node_index (const char *node)
{
    for (size_t i = 0; i < inf_fs_node_count (); i++)
        if (strcmp (node, node_names[i]) == 0)
            return (int) i;
    return -1;
}

bool
inf_task_from_name (const char *s, size_t len, enum inf_task *out)
{
    static const enum inf_task all[] =
    {
        INF_TASK_SUMMARIZE, INF_TASK_TRANSLATE, INF_TASK_ANSWER,
        INF_TASK_CLASSIFY, INF_TASK_CUSTOM,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++)
        if (strlen (inf_task_name (all[i])) == len
            && strncmp (s, inf_task_name (all[i]), len) == 0)
          {
            *out = all[i];
            return true;
          }
    return false;
}

void
inf_fs_init (struct inf_fs *fs)
{
    memset (fs, 0, sizeof *fs);
    fs->status = INF_STATUS_IDLE;
    fs->task = INF_TASK_CUSTOM;
}

int
inf_fs_write (struct inf_fs *fs, const char *node, const char *data,
              size_t len)
{
    if (node_index (node) < 0)
        return -1;

    if (strcmp (node, INF_NODE_PROMPT) == 0)
      {
        /* `echo ... | tee /inference/prompt` appends a newline: trim
           trailing whitespace before storing and parsing. */
        while (len > 0 && (data[len - 1] == '\n' || data[len - 1] == '\r'
                           || data[len - 1] == ' ' || data[len - 1] == '\t'))
            len--;
        if (len >= INF_PROMPT_MAX)
            len = INF_PROMPT_MAX - 1;
        memcpy (fs->prompt, data, len);
        fs->prompt[len] = '\0';
        fs->prompt_len = len;

        inf_parse_prompt (fs->prompt, &fs->last);
        if (fs->task_overridden)
            fs->last.task = fs->task;   /* the manual override wins */
        else
            fs->task = fs->last.task;   /* re-detect on each new prompt */
        fs->parsed = true;
        fs->status = INF_STATUS_PARSED;
        return 0;
      }

    if (strcmp (node, INF_NODE_TASK) == 0)
      {
        /* `tee` appends a newline: trim trailing whitespace. */
        while (len > 0 && (data[len - 1] == '\n' || data[len - 1] == '\r'
                           || data[len - 1] == ' ' || data[len - 1] == '\t'))
            len--;
        if (len == 4 && memcmp (data, "auto", 4) == 0)
          {
            /* "auto" clears a manual override: the task is detected
               again from the current prompt (if one was parsed). */
            fs->task_overridden = false;
            if (fs->parsed)
              {
                fs->task = inf_detect_task (fs->prompt);
                fs->last.task = fs->task;
              }
            return 0;
          }
        enum inf_task t;
        if (!inf_task_from_name (data, len, &t))
            return -1;
        fs->task = t;
        fs->task_overridden = true;
        if (fs->parsed)
            fs->last.task = t;
        return 0;
      }

    return -1;                          /* read-only node */
}

/* Bounded emitter: counts the full length, writes only what fits and
 * keeps room for the final NUL. */
struct emit
{
    char *p;
    size_t cap;
    size_t len;
};

static void
emit_raw (struct emit *e, const void *s, size_t n)
{
    size_t done = e->len;
    e->len += n;
    if (e->p == NULL || e->cap == 0)
        return;
    if (e->len <= e->cap - 1)
        memcpy (e->p + done, s, n);
    else if (done < e->cap - 1)
        memcpy (e->p + done, s, e->cap - 1 - done);
}

static void
emit_str (struct emit *e, const char *s)
{
    emit_raw (e, s, strlen (s));
}

static ssize_t
render_node (struct inf_fs *fs, const char *node, char *buf, size_t bufsz)
{
    struct emit e = { buf, bufsz, 0 };

    if (strcmp (node, INF_NODE_PROMPT) == 0)
        emit_raw (&e, fs->prompt, fs->prompt_len);
    else if (strcmp (node, INF_NODE_REQUEST) == 0)
      {
        if (!fs->parsed)
            return -1;
        char json[INF_JSON_MAX];
        inf_request_to_json (&fs->last, json, sizeof json);
        emit_str (&e, json);
      }
    else if (strcmp (node, INF_NODE_URLS) == 0)
      {
        if (!fs->parsed)
            return -1;
        for (size_t i = 0; i < fs->last.url_count; i++)
          {
            emit_str (&e, fs->last.urls[i]);
            emit_raw (&e, "\n", 1);
          }
      }
    else if (strcmp (node, INF_NODE_TASK) == 0)
        emit_str (&e, inf_task_name (fs->task));
    else if (strcmp (node, INF_NODE_PARAMS) == 0)
        emit_str (&e, "{}");
    else if (strcmp (node, INF_NODE_STATUS) == 0)
        emit_str (&e, inf_status_name (fs->status));
    else if (strcmp (node, INF_NODE_VERSION) == 0)
      {
        char v[160];
        snprintf (v, sizeof v,
                  "{\"program\":\"inference-translator\",\"version\":\"%s\","
                  "\"contract\":%d}",
                  VERSION, INF_CONTRACT_VERSION);
        emit_str (&e, v);
      }
    else
        return -1;

    if (e.p != NULL && e.cap > 0)
        e.p[e.len < e.cap - 1 ? e.len : e.cap - 1] = '\0';
    return (ssize_t) e.len;
}

ssize_t
inf_fs_read (struct inf_fs *fs, const char *node, char *buf, size_t bufsz)
{
    if (node_index (node) < 0)
        return -1;
    return render_node (fs, node, buf, bufsz);
}
