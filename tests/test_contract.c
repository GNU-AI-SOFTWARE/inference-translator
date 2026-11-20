/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * test_contract.c -- Contract tests of the phase 0 core (SPEC.md).
 *
 * Homemade harness, same convention as neuron-translator: a check ()
 * macro-like function, TAP-ish output, counters, no external
 * framework.  The tests describe the frozen contract (the type 1 JSON
 * of SPEC.md section 1, the URL regex and task rules of section 3,
 * the spinner data of section 8), never the implementation.
 *
 * Pure POSIX, no terminal, no ncurses: runs on any machine.
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

static struct inf_request req;

static void
parse (const char *prompt)
{
    inf_parse_prompt (prompt, &req);
}

int
main (void)
{
    char esc[256];
    char json[INF_JSON_MAX];

    /* --- URL extraction and punctuation trimming ----------------- */

    parse ("résume https://example.org/article, stp");
    check ("one URL extracted", req.url_count == 1);
    check ("trailing comma trimmed",
           req.url_count == 1
           && strcmp (req.urls[0], "https://example.org/article") == 0);

    parse ("voir https://ex.org/x! maintenant");
    check ("trailing '!' trimmed",
           req.url_count == 1 && strcmp (req.urls[0], "https://ex.org/x") == 0);

    parse ("compare https://a.io et https://b.io");
    check ("two URLs kept in order",
           req.url_count == 2
           && strcmp (req.urls[0], "https://a.io") == 0
           && strcmp (req.urls[1], "https://b.io") == 0);

    parse ("bonjour le monde");
    check ("no URL in a plain prompt", req.url_count == 0);

    parse ("va sur http://plain.example/page");
    check ("http:// matched too",
           req.url_count == 1
           && strcmp (req.urls[0], "http://plain.example/page") == 0);

    parse ("dit \"https://a.b/c\" ici");
    check ("URL stops at a double quote",
           req.url_count == 1 && strcmp (req.urls[0], "https://a.b/c") == 0);

    parse ("voir <https://a.b/d> merci");
    check ("URL stops at '>'",
           req.url_count == 1 && strcmp (req.urls[0], "https://a.b/d") == 0);

    parse ("https://u0.io https://u1.io https://u2.io https://u3.io"
           " https://u4.io https://u5.io https://u6.io https://u7.io"
           " https://u8.io https://u9.io");
    check ("URL count capped at INF_URL_MAX", req.url_count == INF_URL_MAX);

    /* --- Task detection (frozen keyword table) ------------------- */

    parse ("résume ce texte");
    check ("accented keyword 'résume'", req.task == INF_TASK_SUMMARIZE);
    parse ("resume this please");
    check ("unaccented keyword 'resume'", req.task == INF_TASK_SUMMARIZE);
    parse ("please summarize the doc");
    check ("english keyword 'summarize'", req.task == INF_TASK_SUMMARIZE);
    parse ("traduis ce texte en anglais");
    check ("keyword 'traduis'", req.task == INF_TASK_TRANSLATE);
    parse ("réponds à la question");
    check ("keyword 'réponds'", req.task == INF_TASK_ANSWER);
    parse ("classe ce document");
    check ("keyword 'classe'", req.task == INF_TASK_CLASSIFY);
    parse ("bonjour");
    check ("no keyword falls back to custom", req.task == INF_TASK_CUSTOM);

    /* --- Frozen names -------------------------------------------- */

    check ("task name 'summarize'",
           strcmp (inf_task_name (INF_TASK_SUMMARIZE), "summarize") == 0);
    check ("task name 'custom'",
           strcmp (inf_task_name (INF_TASK_CUSTOM), "custom") == 0);

    /* --- JSON escaping ------------------------------------------- */

    inf_json_escape ("a\"b", esc, sizeof esc);
    check ("double quote escaped", strcmp (esc, "a\\\"b") == 0);
    inf_json_escape ("a\\b", esc, sizeof esc);
    check ("backslash escaped", strcmp (esc, "a\\\\b") == 0);
    inf_json_escape ("a\nb\tc", esc, sizeof esc);
    check ("newline and tab escaped", strcmp (esc, "a\\nb\\tc") == 0);
    inf_json_escape ("\x01", esc, sizeof esc);
    check ("control char as \\u00XX", strcmp (esc, "\\u0001") == 0);
    inf_json_escape ("\xc3\xa9", esc, sizeof esc);
    check ("UTF-8 bytes pass through", strcmp (esc, "\xc3\xa9") == 0);

    /* --- Exact type 1 JSON (SPEC.md section 1) ------------------- */

    parse ("résumé https://example.org/article");
    inf_request_to_json (&req, json, sizeof json);
    check ("exact type 1 JSON, summarize + one URL",
           strcmp (json, "{\"prompt\":\"résumé https://example.org/article\","
                          "\"task\":\"summarize\","
                          "\"urls\":[\"https://example.org/article\"],"
                          "\"params\":{}}") == 0);

    parse ("bonjour");
    inf_request_to_json (&req, json, sizeof json);
    check ("exact type 1 JSON, custom + no URL",
           strcmp (json, "{\"prompt\":\"bonjour\",\"task\":\"custom\","
                          "\"urls\":[],\"params\":{}}") == 0);

    parse ("compare https://a.io et https://b.io");
    inf_request_to_json (&req, json, sizeof json);
    check ("exact type 1 JSON, two URLs",
           strcmp (json, "{\"prompt\":\"compare https://a.io et https://b.io\","
                          "\"task\":\"custom\","
                          "\"urls\":[\"https://a.io\",\"https://b.io\"],"
                          "\"params\":{}}") == 0);

    parse ("dis \"oui\"");
    inf_request_to_json (&req, json, sizeof json);
    check ("quote inside the prompt is escaped in the JSON",
           strcmp (json, "{\"prompt\":\"dis \\\"oui\\\"\",\"task\":\"custom\","
                          "\"urls\":[],\"params\":{}}") == 0);

    /* --- Spinner data (SPEC.md section 8) ------------------------ */

    bool alpha = true;
    for (size_t f = 0; f < INF_SPINNER_FRAMES; f++)
        for (size_t r = 0; r < INF_SPINNER_SIZE; r++)
            for (size_t c = 0; c < INF_SPINNER_SIZE; c++)
                if (strchr ("#s|.", inf_spinner[f][r][c]) == NULL)
                    alpha = false;
    check ("spinner alphabet is '#', 's', '|', '.'", alpha);

    bool dims = true;
    for (size_t f = 0; f < INF_SPINNER_FRAMES; f++)
        for (size_t r = 0; r < INF_SPINNER_SIZE; r++)
            if (strlen (inf_spinner[f][r]) != INF_SPINNER_SIZE)
                dims = false;
    check ("spinner is 10 frames of 7x7", dims);

    bool thread = true;
    for (size_t f = 0; f < INF_SPINNER_FRAMES; f++)
        if (inf_spinner[f][3][3] != '|')
            thread = false;
    check ("thread in the middle column of every frame", thread);

    bool glass = true;
    for (size_t f = 0; f < INF_SPINNER_FRAMES; f++)
      {
        for (size_t c = 0; c < INF_SPINNER_SIZE; c++)
          if (inf_spinner[f][0][c] != '#' || inf_spinner[f][6][c] != '#')
            glass = false;
        for (size_t r = 0; r < INF_SPINNER_SIZE; r++)
          if (inf_spinner[f][r][0] != '#' || inf_spinner[f][r][6] != '#')
            glass = false;
      }
    check ("glass border around every frame", glass);

    bool top_full = true;
    for (size_t c = 1; c <= 5; c++)
        if (inf_spinner[0][1][c] != 's')
            top_full = false;
    for (size_t c = 2; c <= 4; c++)
        if (inf_spinner[0][2][c] != 's')
            top_full = false;
    check ("frame 0 is full on top", top_full);

    bool bottom_full = true;
    for (size_t c = 2; c <= 4; c++)
        if (inf_spinner[INF_SPINNER_FRAMES - 1][4][c] != 's')
            bottom_full = false;
    for (size_t c = 1; c <= 5; c++)
        if (inf_spinner[INF_SPINNER_FRAMES - 1][5][c] != 's')
            bottom_full = false;
    check ("last frame is full at the bottom", bottom_full);

    bool downward = true;
    size_t prev = INF_SPINNER_SIZE * INF_SPINNER_SIZE;
    for (size_t f = 0; f < INF_SPINNER_FRAMES; f++)
      {
        size_t top_sand = 0;
        for (size_t r = 0; r <= 3; r++)
            for (size_t c = 0; c < INF_SPINNER_SIZE; c++)
                if (inf_spinner[f][r][c] == 's')
                    top_sand++;
        if (top_sand > prev)
            downward = false;
        prev = top_sand;
      }
    check ("sand only flows downward", downward);

    bool conserved = true;
    for (size_t f = 0; f < INF_SPINNER_FRAMES; f++)
      {
        size_t sand = 0;
        for (size_t r = 0; r < INF_SPINNER_SIZE; r++)
            for (size_t c = 0; c < INF_SPINNER_SIZE; c++)
                if (inf_spinner[f][r][c] == 's')
                    sand++;
        if (sand != 8)
            conserved = false;
      }
    check ("sand conserved (8 grains in every frame)", conserved);

    /* --- Frozen tables ------------------------------------------- */

    bool actions_ok = inf_actions_count == 9;
    for (size_t i = 0; i < inf_actions_count; i++)
        if (inf_actions[i].command[0] != '/')
            actions_ok = false;
    check ("frozen tables: 9 slash commands, expressions, task rules",
           actions_ok
           && inf_exprs_count >= 1
           && strcmp (inf_exprs[0].pattern, INF_URL_REGEX) == 0
           && inf_task_rules_count == 4);

    printf ("%d/%d OK\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
