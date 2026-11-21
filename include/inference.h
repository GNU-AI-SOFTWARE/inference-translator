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
#include <sys/types.h>

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

/* --- The /inference translator state (phase 1) ----------------------
 *
 * Pure C23/POSIX: this is the engine the GNU/Hurd netfs glue
 * (netfs-hooks.c) maps onto the frozen tree, and what the unit tests
 * drive directly — no terminal, no Hurd library needed. */

#define INF_CONTRACT_VERSION 1
#define INF_PROMPT_MAX 8192

struct inf_fs
{
    char prompt[INF_PROMPT_MAX];        /* last raw prompt, NUL-terminated */
    size_t prompt_len;
    enum inf_status status;
    enum inf_task task;                 /* effective task (override wins) */
    bool task_overridden;               /* /inference/task was written */
    struct inf_request last;            /* parsed request of last prompt */
    bool parsed;
};

void inf_fs_init (struct inf_fs *fs);

/* The frozen node names of /inference (SPEC.md section 2), in order. */
const char *const *inf_fs_nodes (void);
size_t inf_fs_node_count (void);

/* Write to a node: only "prompt" and "task" accept writes.  Returns 0
 * on success, -1 on error (unknown node, read-only node, invalid task
 * name). */
int inf_fs_write (struct inf_fs *fs, const char *node,
                  const char *data, size_t len);

/* Read a node.  Returns the full content length (>= 0) and writes at
 * most bufsz bytes, NUL-terminated when bufsz > 0; pass buf == NULL
 * (or bufsz == 0) to query the length only.  Returns -1 for an
 * unknown node, or for "request"/"urls" before any prompt was
 * written. */
ssize_t inf_fs_read (struct inf_fs *fs, const char *node,
                     char *buf, size_t bufsz);

/* --- ncurses CLI prototype (src/cli.c, only when HAVE_NCURSES) ----- */

#ifdef HAVE_NCURSES
int inf_cli_run (void);
#endif

/* --- Phase 2: the `inference` client (SPEC.md section 12) ----------
 *
 * Pure, testable core (src/client-core.c): editor resolution, slash
 * command parsing, narration formatting, NO_COLOR handling.  The
 * interactive binary (src/client.c) drives /inference through plain
 * POSIX open/read/write. */

/* Slash commands of the frozen inf_actions[] table. */
enum inf_client_cmd
{
    INF_CMD_NONE,                       /* not a command line */
    INF_CMD_EDITOR,
    INF_CMD_TASK,
    INF_CMD_SUBMIT,
    INF_CMD_STATUS,
    INF_CMD_FETCH,
    INF_CMD_STATS,
    INF_CMD_HELP,
    INF_CMD_QUIT,
    INF_CMD_LEARN,
    INF_CMD_READ_CODE,
    INF_CMD_LEARN_CODE,
    INF_CMD_UNKNOWN
};

/* Frozen editor resolution (SPEC.md section 5):
 * $VISUAL -> $EDITOR -> nano -> vi.  Returns a static/constant string
 * naming the editor to exec (never NULL). */
const char *inf_client_editor (void);

/* Parse one input line as a slash command.  Returns INF_CMD_NONE for
 * plain text; for INF_CMD_TASK the argument is copied into arg. */
enum inf_client_cmd inf_client_parse_command (const char *line,
                                              char *arg, size_t argsz);

/* Tab completion for the REPL line editor: completes a slash command
 * ("/st<TAB>") or the argument of "/task " against the frozen tables.
 * Returns the number of matches; with exactly one match OUT holds the
 * completed line (NUL-terminated).  LIST receives the match names, up
 * to list_max entries. */
size_t inf_client_complete (const char *line, size_t len, char *out,
                            size_t outsz, const char **list,
                            size_t list_max);

/* Validate a task name against the frozen enum. */
bool inf_task_from_name (const char *s, size_t len, enum inf_task *out);

/* True unless NO_COLOR is set and non-empty (SPEC.md section 6). */
bool inf_client_color (void);

/* Narration line for one step, naming the POSIX node involved:
 * e.g. "write /inference/prompt".  Returns the length written. */
size_t inf_client_narrate (char *out, size_t outsz, const char *verb,
                           const char *mount, const char *node);

/* Colored banner line (ANSI only when inf_client_color ()). */
size_t inf_client_banner (char *out, size_t outsz, const char *version);

/* One /stats snapshot: CPU %, memory and I/O counters, as a small
 * multi-line text block (NUL-terminated, bounded by outsz).  Sources:
 * /proc on Linux, Mach host statistics on Hurd (I/O then "n/d").
 * Returns the length written. */
size_t inf_client_stats (char *out, size_t outsz);

/* Phase 3 highlighting (SPEC.md section 6): pure renderers, no TTY.
 * They emit ANSI codes only when COLOR is true; with color=false the
 * output is the input copied verbatim.  Both are bounded by outsz and
 * NUL-terminate; they return the length written (excluding the NUL).
 *
 * inf_json_highlight: JSON syntax - object keys bold cyan, strings
 * green, numbers magenta, true/false/null yellow, punctuation dim.
 * inf_text_highlight: prompt text - http(s):// URLs underlined cyan,
 * **bold**, *italic* and `code` in light markdown. */
size_t inf_json_highlight (char *out, size_t outsz, const char *json,
                           bool color);
size_t inf_text_highlight (char *out, size_t outsz, const char *text,
                           bool color);

/* A fetchable URL: http:// or https:// followed by a non-empty host
 * (the frozen INF_URL_REGEX requires a non-space run after the
 * scheme; here we additionally reject a bare "/" or "?" as host). */
bool inf_url_valid (const char *url);

/* Strip trailing whitespace and sentence punctuation (",;:.!?)...]"
 * quotes) from an URL in place - a prompt line often ends with a comma.
 * Returns the new length. */
size_t inf_url_trim (char *url);

/* Training element built from a fetched page (data-base-translator
 * contract, schema.sql `training_data`): SHA-256 of the extracted
 * text as anti-duplicate checksum, then the JSON write payload for
 * /db.  All pure and bounded. */
void inf_sha256_hex (char out[65], const unsigned char *data, size_t len);
size_t inf_html_to_text (char *out, size_t outsz, const char *html);
size_t inf_training_record (char *out, size_t outsz, const char *url,
                            long http_status, const char *text,
                            const char *checksum_hex);

/* /read-code: detect the programming language of a fetched page from
 * the URL extension, else the shebang, else "text".  Returns a static
 * language name (never NULL). */
const char *inf_lang_detect (const char *url, const char *content);

/* /read-code: render source code with ANSI colors when COLOR is true
 * (comments dim, strings green, numbers magenta, keywords bold blue);
 * verbatim copy when false.  Bounded by outsz, NUL-terminated. */
size_t inf_code_highlight (char *out, size_t outsz, const char *code,
                           const char *lang, bool color);

#endif /* INFERENCE_H */
