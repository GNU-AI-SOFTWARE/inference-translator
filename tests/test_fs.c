/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * test_fs.c -- Unit tests of the /inference translator engine (fs.c).
 *
 * Same homemade harness as test_contract.c: the tests describe the
 * frozen contract (SPEC.md sections 2 and 11), never the
 * implementation.  Pure POSIX: runs on any machine, Hurd or not.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "inference.h"

static int g_checks = 0;
static int g_failed = 0;

static void
check (const char *name, bool ok)
{
    g_checks++;
    if (!ok)
        g_failed++;
    printf ("%s %d - %s\n", ok ? "ok" : "not ok", g_checks, name);
}

static struct inf_fs fs;
static char buf[INF_JSON_MAX];

static ssize_t
read_node (const char *node)
{
    return inf_fs_read (&fs, node, buf, sizeof buf);
}

int
main (void)
{
    inf_fs_init (&fs);

    /* --- Initial state ------------------------------------------- */

    check ("initial status is idle",
           read_node (INF_NODE_STATUS) == 4
           && strcmp (buf, "idle") == 0);
    check ("request unreadable before any prompt",
           inf_fs_read (&fs, INF_NODE_REQUEST, buf, sizeof buf) == -1);
    check ("urls unreadable before any prompt",
           inf_fs_read (&fs, INF_NODE_URLS, buf, sizeof buf) == -1);
    check ("the tree has exactly 7 nodes", inf_fs_node_count () == 7);
    check ("node names are the frozen ones",
           strcmp (inf_fs_nodes ()[0], "prompt") == 0
           && strcmp (inf_fs_nodes ()[6], "version") == 0);

    /* --- write prompt -> parsed request --------------------------- */

    check ("write prompt accepted",
           inf_fs_write (&fs, INF_NODE_PROMPT,
                         "résume https://example.org/article",
                         strlen ("résume https://example.org/article")) == 0);
    check ("status becomes parsed after a prompt",
           read_node (INF_NODE_STATUS) == 6
           && strcmp (buf, "parsed") == 0);
    check ("request is the exact type 1 JSON",
           read_node (INF_NODE_REQUEST) >= 0
           && strcmp (buf, "{\"prompt\":\"résume https://example.org/article\","
                          "\"task\":\"summarize\","
                          "\"urls\":[\"https://example.org/article\"],"
                          "\"params\":{}}") == 0);
    check ("urls are one per line",
           read_node (INF_NODE_URLS) >= 0
           && strcmp (buf, "https://example.org/article\n") == 0);
    check ("task detected from the prompt",
           read_node (INF_NODE_TASK) == 9
           && strcmp (buf, "summarize") == 0);
    check ("params stays empty in phase 1",
           read_node (INF_NODE_PARAMS) == 2 && strcmp (buf, "{}") == 0);
    check ("version node carries the contract version",
           read_node (INF_NODE_VERSION) >= 0
           && strcmp (buf, "{\"program\":\"inference-translator\","
                          "\"version\":\"0.1.0\",\"contract\":1}") == 0);
    check ("prompt reads back what was written",
           read_node (INF_NODE_PROMPT)
               == (ssize_t) strlen ("résume https://example.org/article")
           && strcmp (buf, "résume https://example.org/article") == 0);
    read_node (INF_NODE_REQUEST);       /* buf = request again */
    check ("size query (NULL buffer) matches the rendered length",
           inf_fs_read (&fs, INF_NODE_REQUEST, NULL, 0)
           == (ssize_t) strlen (buf));

    /* --- /inference/task override --------------------------------- */

    check ("task override accepted (trailing newline trimmed)",
           inf_fs_write (&fs, INF_NODE_TASK, "translate\n", 10) == 0);
    check ("task reads back as translate",
           read_node (INF_NODE_TASK) == 9
           && strcmp (buf, "translate") == 0);
    check ("the request carries the overridden task",
           read_node (INF_NODE_REQUEST) >= 0
           && strcmp (buf, "{\"prompt\":\"résume https://example.org/article\","
                          "\"task\":\"translate\","
                          "\"urls\":[\"https://example.org/article\"],"
                          "\"params\":{}}") == 0);
    check ("invalid task name refused",
           inf_fs_write (&fs, INF_NODE_TASK, "bogus", 5) == -1);
    check ("task unchanged after an invalid write",
           read_node (INF_NODE_TASK) == 9
           && strcmp (buf, "translate") == 0);

    /* --- the override is sticky across prompts -------------------- */

    check ("new prompt keeps the manual task override",
           inf_fs_write (&fs, INF_NODE_PROMPT, "bonjour le monde", 16) == 0
           && read_node (INF_NODE_TASK) == 9
           && strcmp (buf, "translate") == 0);
    check ("request reflects the new prompt and the override",
           read_node (INF_NODE_REQUEST) >= 0
           && strcmp (buf, "{\"prompt\":\"bonjour le monde\","
                          "\"task\":\"translate\",\"urls\":[],"
                          "\"params\":{}}") == 0);
    check ("override can be set back to custom",
           inf_fs_write (&fs, INF_NODE_TASK, "custom", 6) == 0
           && read_node (INF_NODE_TASK) == 6
           && strcmp (buf, "custom") == 0);

    /* --- read-only and unknown nodes ------------------------------ */

    check ("writing a read-only node is refused",
           inf_fs_write (&fs, INF_NODE_REQUEST, "x", 1) == -1);
    check ("writing an unknown node is refused",
           inf_fs_write (&fs, "nope", "x", 1) == -1);
    check ("reading an unknown node fails",
           inf_fs_read (&fs, "nope", buf, sizeof buf) == -1);

    /* --- several URLs --------------------------------------------- */

    inf_fs_write (&fs, INF_NODE_PROMPT,
                  "compare https://a.io et https://b.io", 36);
    check ("several urls, one per line, in order",
           read_node (INF_NODE_URLS) >= 0
           && strcmp (buf, "https://a.io\nhttps://b.io\n") == 0);
    check ("request with two urls is exact",
           read_node (INF_NODE_REQUEST) >= 0
           && strcmp (buf, "{\"prompt\":\"compare https://a.io et https://b.io\","
                          "\"task\":\"custom\","
                          "\"urls\":[\"https://a.io\",\"https://b.io\"],"
                          "\"params\":{}}") == 0);

    /* --- bounded read ---------------------------------------------- */

    ssize_t full = inf_fs_read (&fs, INF_NODE_REQUEST, NULL, 0);
    char small[8];
    ssize_t got = inf_fs_read (&fs, INF_NODE_REQUEST, small, sizeof small);
    check ("small buffer: full length returned, output truncated and NUL-terminated",
           got == full && full > 7
           && strncmp (small, buf, 7) == 0 && small[7] == '\0');

    /* --- prompt write trims the newline that tee appends ---------- */

    check ("prompt write trims the trailing newline from tee",
           inf_fs_write (&fs, INF_NODE_PROMPT, "bonjour\n", 8) == 0
           && read_node (INF_NODE_PROMPT) == 7
           && strcmp (buf, "bonjour") == 0);

    printf ("%d/%d OK\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
