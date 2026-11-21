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

    /* --- /learn: training elements (schema.sql training_data) ------ */

    char sum[65];
    inf_sha256_hex (sum, (const unsigned char *) "abc", 3);
    check ("sha256(\"abc\") known vector",
           strcmp (sum, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    inf_sha256_hex (sum, (const unsigned char *) "", 0);
    check ("sha256(\"\") known vector",
           strcmp (sum, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);

    char txt[4096];
    size_t tn = inf_html_to_text (txt, sizeof txt,
        "<p>Hello <b>world</b> &amp; all</p><script>var x=1;</script>");
    check ("html_to_text strips tags/scripts and decodes entities",
           strcmp (txt, "Hello world & all") == 0);

    char rec[8192];
    inf_training_record (rec, sizeof rec, "https://example.org/a",
                         200, "Hello \"world\"\n", sum);
    check ("training record carries table, row, url, status, checksum",
           strstr (rec, "\"table\":\"training_data\"") != NULL
           && strstr (rec, "\"source_url\":\"https://example.org/a\"") != NULL
           && strstr (rec, "\"http_status\":200") != NULL
           && strstr (rec, "\"checksum\":\"") != NULL
           && strstr (rec, "Hello \\\"world\\\"") != NULL);

    char larg[256];
    check ("/learn parses as a command with its URL argument",
           inf_client_parse_command ("/learn https://example.org",
                                     larg, sizeof larg) == INF_CMD_LEARN
           && strcmp (larg, "https://example.org") == 0);

    /* --- /read-code: language detection + code highlighting -------- */

    check ("lang from .py extension",
           strcmp (inf_lang_detect ("https://x.org/a/b.py", NULL), "python") == 0);
    check ("lang from .c extension with query string",
           strcmp (inf_lang_detect ("https://x.org/src/main.c?v=2", NULL), "c") == 0);
    check ("lang from shebang when no extension",
           strcmp (inf_lang_detect ("https://x.org/raw/tool", "#!/usr/bin/env python3\n"), "python") == 0);
    check ("lang fallback is text",
           strcmp (inf_lang_detect ("https://x.org/", "hello world"), "text") == 0);

    const char *src = "# comment\nreturn \"hi\" + 42\n";
    hn = inf_code_highlight (hl, sizeof hl, src, "python", false);
    check ("code highlight color=false copies verbatim",
           hn == strlen (src) && strcmp (hl, src) == 0);
    hn = inf_code_highlight (hl, sizeof hl, src, "python", true);
    check ("code highlight colors comments, strings, numbers, keywords",
           strstr (hl, "\033[90m# comment\033[0m") != NULL
           && strstr (hl, "\033[32m\"hi\"\033[0m") != NULL
           && strstr (hl, "\033[35m42\033[0m") != NULL
           && strstr (hl, "\033[1;34mreturn\033[0m") != NULL);

    check ("/read-code parses as a command with its URL argument",
           inf_client_parse_command ("/read-code https://x.org/a.c",
                                     larg, sizeof larg) == INF_CMD_READ_CODE
           && strcmp (larg, "https://x.org/a.c") == 0);

    /* --- URL trimming (trailing punctuation from prompt lines) ------ */

    char ub[256];
    snprintf (ub, sizeof ub, "https://deb.debian.org,");
    inf_url_trim (ub);
    check ("trailing comma is trimmed from URLs",
           strcmp (ub, "https://deb.debian.org") == 0);
    snprintf (ub, sizeof ub, "https://x.org/a.py. \t");
    inf_url_trim (ub);
    check ("trailing dot/space trimmed, path kept",
           strcmp (ub, "https://x.org/a.py") == 0);

    printf ("%d/%d OK\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
