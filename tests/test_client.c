/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * test_client.c -- Unit tests of the pure core of the phase 2
 * `inference` client (src/client-core.c): editor resolution, slash
 * command parsing, task validation, NO_COLOR, narration, banner.
 * No TTY, no filesystem: runs anywhere (SPEC.md section 9 style).
 */

#define _POSIX_C_SOURCE 200809L

#include "inference.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks;
static int g_failed;

#define check(name, ok) do {                                   \
        g_checks++;                                            \
        if (ok)                                                \
            printf ("ok %d - %s\n", g_checks, name);           \
        else                                                   \
          {                                                    \
            g_failed++;                                        \
            printf ("not ok %d - %s\n", g_checks, name);       \
          }                                                    \
    } while (0)

int
main (void)
{
    /* --- editor resolution: $VISUAL -> $EDITOR -> nano -> vi ----- */

    setenv ("VISUAL", "myvis", 1);
    setenv ("EDITOR", "myed", 1);
    check ("$VISUAL wins", strcmp (inf_client_editor (), "myvis") == 0);
    unsetenv ("VISUAL");
    check ("$EDITOR next", strcmp (inf_client_editor (), "myed") == 0);
    unsetenv ("EDITOR");
    const char *def = inf_client_editor ();
    check ("fallback is nano or vi",
           strcmp (def, "nano") == 0 || strcmp (def, "vi") == 0);

    /* --- slash command parsing ------------------------------------ */

    char arg[256];
    check ("plain text is not a command",
           inf_client_parse_command ("bonjour", arg, sizeof arg)
               == INF_CMD_NONE);
    check ("/quit", inf_client_parse_command ("/quit", arg, sizeof arg)
           == INF_CMD_QUIT);
    check ("/editor", inf_client_parse_command ("/editor", arg, sizeof arg)
           == INF_CMD_EDITOR);
    check ("/submit", inf_client_parse_command ("/submit", arg, sizeof arg)
           == INF_CMD_SUBMIT);
    check ("/status", inf_client_parse_command ("/status", arg, sizeof arg)
           == INF_CMD_STATUS);
    check ("/task translate carries the argument",
           inf_client_parse_command ("/task translate", arg, sizeof arg)
               == INF_CMD_TASK && strcmp (arg, "translate") == 0);
    check ("/task with extra spaces trims the argument",
           inf_client_parse_command ("/task   answer  ", arg, sizeof arg)
               == INF_CMD_TASK && strcmp (arg, "answer") == 0);
    check ("unknown slash command",
           inf_client_parse_command ("/nope", arg, sizeof arg)
               == INF_CMD_UNKNOWN);

    /* --- task name validation (shared with /inference/task) ------- */

    enum inf_task t;
    check ("summarize is a valid task",
           inf_task_from_name ("summarize", 9, &t) && t == INF_TASK_SUMMARIZE);
    check ("custom is a valid task",
           inf_task_from_name ("custom", 6, &t) && t == INF_TASK_CUSTOM);
    check ("bogus is refused", !inf_task_from_name ("bogus", 5, &t));

    /* --- NO_COLOR --------------------------------------------------- */

    unsetenv ("NO_COLOR");
    check ("color on by default", inf_client_color ());
    setenv ("NO_COLOR", "1", 1);
    check ("NO_COLOR=1 disables color", !inf_client_color ());
    setenv ("NO_COLOR", "", 1);
    check ("empty NO_COLOR keeps color", inf_client_color ());
    unsetenv ("NO_COLOR");

    /* --- narration names the POSIX node ---------------------------- */

    char line[4200];
    size_t n = inf_client_narrate (line, sizeof line, "write",
                                   "/inference", INF_NODE_PROMPT);
    check ("narration names verb + mount + node",
           n == strlen ("write /inference/prompt")
           && strcmp (line, "write /inference/prompt") == 0);

    /* --- banner: ANSI only with color ------------------------------- */

    char banner[256];
    inf_client_banner (banner, sizeof banner, "0.1.0");
    check ("banner carries the name and version",
           strstr (banner, "inference") != NULL
           && strstr (banner, "0.1.0") != NULL && strchr (banner, '\033') != NULL);
    setenv ("NO_COLOR", "1", 1);
    inf_client_banner (banner, sizeof banner, "0.1.0");
    check ("NO_COLOR banner has no ANSI escape",
           strchr (banner, '\033') == NULL && strstr (banner, "inference") != NULL);
    unsetenv ("NO_COLOR");

    /* --- phase 3: syntax highlighting ------------------------------ */

    char hl[8192];
    const char *js = "{\"prompt\":\"résume https://example.org\",\"task\":\"summarize\",\"n\":2,\"ok\":true,\"x\":null}";
    size_t hn = inf_json_highlight (hl, sizeof hl, js, false);
    check ("json highlight color=false copies verbatim",
           hn == strlen (js) && strcmp (hl, js) == 0);
    hn = inf_json_highlight (hl, sizeof hl, js, true);
    check ("json highlight color=true emits ANSI and keeps the text",
           strchr (hl, '\033') != NULL
           && strstr (hl, "\"prompt\"") != NULL
           && strstr (hl, "summarize") != NULL
           && strstr (hl, "true") != NULL);
    check ("json highlight marks object keys differently from values",
           strstr (hl, "\033[1;36m\"prompt\"") != NULL
           && strstr (hl, "\033[32m\"summarize\"") != NULL);

    const char *tx = "voir https://example.org/a **en gras** et `code` ici";
    hn = inf_text_highlight (hl, sizeof hl, tx, false);
    check ("text highlight color=false copies verbatim",
           hn == strlen (tx) && strcmp (hl, tx) == 0);
    hn = inf_text_highlight (hl, sizeof hl, tx, true);
    check ("text highlight underlines URLs and bolds markdown",
           strstr (hl, "\033[4;36mhttps://example.org/a\033[0m") != NULL
           && strstr (hl, "\033[1m**en gras**\033[0m") != NULL
           && strstr (hl, "\033[2m`code`\033[0m") != NULL);

    /* --- /fetch URL validation ------------------------------------- */

    check ("https URL with host is valid",
           inf_url_valid ("https://example.org") && inf_url_valid ("http://93.184.216.34/x"));
    check ("bare scheme or empty host is refused",
           !inf_url_valid ("https://") && !inf_url_valid ("http://")
           && !inf_url_valid ("https:///path") && !inf_url_valid ("ftp://x")
           && !inf_url_valid ("example.org") && !inf_url_valid (""));

    printf ("%d/%d OK\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
