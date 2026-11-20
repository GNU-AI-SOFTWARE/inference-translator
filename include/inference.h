/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * inference.h -- The frozen phase 0 contracts of inference-translator,
 * expressed as C23 constants and pure functions (see SPEC.md).
 *
 * These names are a contract with orchestrator-translator and
 * data-base-translator: do not rename them (SPEC.md section 0).
 */

#ifndef INFERENCE_H
#define INFERENCE_H

#include <stdbool.h>
#include <stddef.h>

/* --- Mount points (frozen) ---------------------------------------- */

#define INF_MOUNT_INFERENCE   "/inference"
#define INF_MOUNT_ORCHESTRATE "/orchestrate"
#define INF_MOUNT_DB          "/db"

/* --- /inference node names (frozen) ------------------------------- */

#define INF_NODE_PROMPT  "prompt"
#define INF_NODE_REQUEST "request"
#define INF_NODE_URLS    "urls"
#define INF_NODE_TASK    "task"
#define INF_NODE_PARAMS  "params"
#define INF_NODE_STATUS  "status"
#define INF_NODE_VERSION "version"

/* --- Task types (frozen, /inference/task) -------------------------- */

enum inf_task
{
    INF_TASK_SUMMARIZE,
    INF_TASK_TRANSLATE,
    INF_TASK_ANSWER,
    INF_TASK_CLASSIFY,
    INF_TASK_CUSTOM
};

const char *inf_task_name (enum inf_task task);

/* --- Interface status (frozen, /inference/status) ----------------- */

enum inf_status
{
    INF_STATUS_IDLE,
    INF_STATUS_COMPOSING,
    INF_STATUS_PARSED,
    INF_STATUS_SUBMITTED
};

const char *inf_status_name (enum inf_status status);

/* --- URL extraction (frozen POSIX ERE, SPEC.md section 3) --------- */

#define INF_URL_REGEX "https?://[^[:space:]\"'<>]+"

/* Trailing sentence punctuation trimmed after extraction: these end
 * a clause, never a host. */
#define INF_URL_TRIM ",.;:!?"

/* Zero allocation in hot paths: the request holds fixed-size arrays
 * filled by the caller-side parser. */
#define INF_URL_MAX 8
#define INF_URL_LEN 1024

/* Bound of the rendered type 1 request JSON; inf_request_to_json ()
 * writes into a caller-provided buffer of at least this size. */
#define INF_JSON_MAX 8192

struct inf_request
{
    const char *prompt;                 /* borrowed, not owned */
    enum inf_task task;
    size_t url_count;
    char urls[INF_URL_MAX][INF_URL_LEN];
};

/* Parse a raw prompt into a structured request: URL extraction with
 * the frozen ERE (trailing punctuation trimmed) and keyword-based
 * task detection (inf_task_rules[]).  Returns req->url_count. */
size_t inf_parse_prompt (const char *prompt, struct inf_request *req);

/* Render the type 1 request JSON, byte-exact:
 * {"prompt":"...","task":"...","urls":[...],"params":{}}
 * The output is always NUL-terminated and bounded by outsz. */
void inf_request_to_json (const struct inf_request *req,
                          char *out, size_t outsz);

/* JSON-escape a UTF-8 string (quotes, backslash, control chars as
 * \u00XX; bytes >= 0x80 pass through).  Bounded by outsz. */
void inf_json_escape (const char *in, char *out, size_t outsz);

/* --- Frozen tables (src/expr.c, src/actions.c) --------------------- */

struct inf_expr
{
    const char *name;
    const char *pattern;
};

extern const struct inf_expr inf_exprs[];
extern const size_t inf_exprs_count;

struct inf_task_rule
{
    enum inf_task task;
    const char *pattern;
};

extern const struct inf_task_rule inf_task_rules[];
extern const size_t inf_task_rules_count;

struct inf_action
{
    const char *command;                /* slash command, e.g. "/quit" */
    const char *help;                   /* one-line description */
};

extern const struct inf_action inf_actions[];
extern const size_t inf_actions_count;

/* --- Pixel hourglass spinner (src/spinner.c, frozen data) --------- */

#define INF_SPINNER_SIZE   7
#define INF_SPINNER_FRAMES 10

/* 10 frames of a 7x7 card, each row a NUL-terminated string of 7
 * chars.  Alphabet: '#' glass, 's' sand, '|' thread, '.' empty.
 * Sand only flows downward: frame 0 is full on top, the last frame
 * is full at the bottom. */
extern const char inf_spinner[INF_SPINNER_FRAMES][INF_SPINNER_SIZE]
                             [INF_SPINNER_SIZE + 1];

/* --- ncurses CLI prototype (src/cli.c, only when HAVE_NCURSES) ----- */

#ifdef HAVE_NCURSES
int inf_cli_run (void);
#endif

#endif /* INFERENCE_H */
