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

/* Maximum raw prompt stored by the /inference node. */
#define INF_PROMPT_MAX 8192

/* Bound of the rendered type 1 request JSON; inf_request_to_json ()
 * writes into a caller-provided buffer of at least this size.  Worst
 * case: every prompt byte escaped as \u00XX (6x) inside "prompt",
 * plus the urls array re-rendering those same bytes (again 6x),
 * plus the fixed skeleton and the NUL. */
#define INF_JSON_MAX (12 * INF_PROMPT_MAX + 64)

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

/* Portable --root DIR engine (src/rootmode.c): serve the /inference
   node tree as plain files under DIR, for tests on hosts where no
   translator can be mounted (and on Hurd without a mount). */
int inf_rootmode_serve (const char *dir);

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
    INF_CMD_WATCH,
    INF_CMD_HISTORY,
    INF_CMD_UPTIME,
    INF_CMD_REPLY,
    INF_CMD_SIGMOID,
    INF_CMD_TRAINING,
    INF_CMD_HF,
    INF_CMD_DB,
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

/* Keyword-rule task detection (frozen inf_task_rules[] table); used to
   re-detect the task when a manual override is cleared with "auto". */
enum inf_task inf_detect_task (const char *prompt);

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

/* Phase 4 (/watch): minimal readers for the frozen orchestrator JSON
 * (types 2 and 3, flat objects as rendered by orchestrator-translator).
 * inf_json_str_field copies the string value of KEY ("key":"value")
 * into OUT (unescaping \" and \\); inf_json_num_field parses the numeric
 * value of KEY with strtod.  Both return 0 on success, -1 when the key
 * is absent or the value has the wrong shape. */
int inf_json_str_field (const char *json, const char *key,
                        char *out, size_t outsz);
int inf_json_num_field (const char *json, const char *key, double *out);
/* inf_json_unescape: decode JSON string escapes in place (\\, \", \/,
 * \b, \f, \n, \r, \t, \uXXXX basic BMP -> UTF-8); returns the new
 * length.  For stored content that was escaped one level more than the
 * surrounding record (JSON-in-JSON). */
size_t inf_json_unescape (char *s);
/* inf_json_strings_extract: pull the string literals out of a (possibly
 * partial) JSON fragment -- e.g. a mid-array chunk of a stored .json
 * source file.  Object keys (a literal followed by ':') and literals
 * without any letter or digit are skipped; each kept literal is
 * unescaped (twice when it still carries JSON-in-JSON escapes) and the
 * results are joined with newlines.  Returns 0 when at least one
 * literal was kept, -1 otherwise. */
int inf_json_strings_extract (const char *json, char *out, size_t outsz);

/* Phase 4: does BUF carry the expected role of a /orchestrate node?
 * In phase 1 (trivfs) every sub-node name serves the same shared slot,
 * so reading /orchestrate/status can return the result JSON; the client
 * uses this to fall back to the {"command":...} protocol.  "status"
 * must carry an "instances" array, "result" an "output" string (or the
 * {"empty": true} marker). */
bool inf_orch_role_ok (const char *node, const char *buf);

/* --- Phase 6: session history, persisted in a .history file --------- */

struct inf_history
{
    char **lines;       /* owned, strdup'd, NUL-terminated */
    size_t count;
    size_t cap;
};

void inf_history_init (struct inf_history *h);
void inf_history_free (struct inf_history *h);
/* Load PATH (one entry per line); empty lines and consecutive
 * duplicates are skipped (HISTCONTROL=ignoredups).  A missing file is
 * not an error. */
void inf_history_load (struct inf_history *h, const char *path);
/* Append LINE (skipped when empty or identical to the last entry).
 * Returns the entry count after the call. */
size_t inf_history_add (struct inf_history *h, const char *line);
/* Entry I (NULL when out of range). */
const char *inf_history_get (const struct inf_history *h, size_t i);
/* Append one line + '\n' to PATH (O_APPEND|O_CREAT, mode 0600).
 * Returns 0 on success, -1 on error. */
int inf_history_append (const char *path, const char *line);

/* /uptime: format SECONDS as a human uptime string, e.g. "05s",
 * "1m 05s", "1h 00m 00s", "1j 01h 01m 01s".  Bounded, NUL-terminated,
 * returns the length written. */
size_t inf_format_uptime (char *out, size_t outsz, long seconds);

/* --- URL host extraction + /etc/hosts helpers (auto-DNS) ------------
 *
 * inf_url_host: copy the host part of URL (after http:// or https://,
 * up to '/', ':', '?' or the end) into HOST.  Returns false when there
 * is no host.  A port is stripped (kept out of HOST). */
bool inf_url_host (const char *url, char *host, size_t hostsz);

/* inf_hosts_has: true when HOSTS_TEXT (an /etc/hosts file content)
 * already maps HOST (a non-comment line whose field list contains the
 * host, so "10.0.0.1 example.org www.example.org" matches both). */
bool inf_hosts_has (const char *hosts_text, const char *host);

/* --- Neuron module (src/neuron.c): sigmoid view + input vector -------
 *
 * inf_sigmoid: the classic 1/(1+e^-x), clamped input to avoid
 * overflow. */
double inf_sigmoid (double x);

/* inf_parse_neuron_output: parse the "Output:" section of a neuron
 * translator status text ("  [3]: 0.123456") into VALS.  Returns the
 * number of values parsed (<= max); *n receives it. */
size_t inf_parse_neuron_output (const char *text, double *vals,
                                size_t max, size_t *n);

/* inf_sigmoid_render: one text block visualizing the sigmoid state of
 * N neuron outputs of LABEL (e.g. "/llm1"): one line per output with
 * an ASCII bar.  Bounded, NUL-terminated, returns the length written. */
size_t inf_sigmoid_render (char *out, size_t outsz, const char *label,
                           const double *vals, size_t n, bool color);

/* inf_text_to_vector: deterministic fixed-size input vector for the
 * neuron translator: byte histogram of the first N buckets normalized
 * to [0,1].  Returns n (always fills the whole vector). */
size_t inf_text_to_vector (const char *text, double *out, size_t n);

/* inf_vector_command: render the "input v1,...,vN" write command for the
 * neuron translator from a vector (comma-separated %.6f), bounded and
 * NUL-terminated; returns the length written. */
size_t inf_vector_command (char *out, size_t outsz, const double *vec,
                           size_t n);

/* inf_vector_to_text: decode a neuron output vector into a short
 * printable string (inverse spirit of inf_text_to_vector); bounded,
 * NUL-terminated, returns the length written. */
size_t inf_vector_to_text (char *out, size_t outsz, const double *vals,
                           size_t n);

/* --- /reply: search the training cache -------------------------------
 *
 * inf_training_search: scan an NDJSON cache of training records (one
 * JSON object per line, as written by /learn, /learn-code and
 * /entrainement), score each record by the number of distinct query
 * words found in its "content" field (case-insensitive), and copy the
 * best matching LINE into OUT.  Returns the length written (0 when no
 * record scores above zero). */
size_t inf_training_search (const char *ndjson, const char *query,
                            char *out, size_t outsz);

/* inf_reply_prompt: bounded grounding prompt for a neuron-backed /reply:
 * "question: QUERY\ncontexte: CONTEXT" with both parts truncated to fit;
 * always NUL-terminated, returns the length written. */
size_t inf_reply_prompt (char *out, size_t outsz, const char *query,
                         const char *context);

/* inf_training_filter_new: copy the lines of NEW_NDJSON whose "checksum"
 * field does not occur in EXISTING_NDJSON into OUT (bounded, NUL-terminated);
 * returns the length written.  Used by /db sync to merge /db records into
 * the local cache without duplicating checksums. */
size_t inf_training_filter_new (char *out, size_t outsz,
                                const char *new_ndjson,
                                const char *existing_ndjson);

/* --- /hf: Hugging Face buckets/datasets over plain HTTP --------------
 *
 * inf_hf_url: true for hf://... URLs (handled by /hf and /entrainement). */
bool inf_hf_url (const char *url);

/*
 * inf_hf_parse: parse hf://[buckets|datasets|models]/<owner>/<repo>[/<path>]
 * into KIND ("buckets"/"datasets"/"models"), OWNER, REPO and PATH
 * (possibly empty).  Returns false on a malformed hf:// URL. */
bool inf_hf_parse (const char *hfurl, char *kind, size_t kindsz,
                   char *owner, size_t osz, char *repo, size_t rsz,
                   char *path, size_t psz);

/* inf_hf_api_url: the HF API URL listing the repo tree (JSON array of
 * {"path":..,"type":..,"size":..}), recursive. */
size_t inf_hf_api_url (const char *hfurl, char *out, size_t outsz);

/* inf_hf_resolve_url: the download URL of one PATH of the repo
 * (huggingface.co resolve endpoint, redirects followed by httpfs). */
size_t inf_hf_resolve_url (const char *hfurl, const char *path,
                           char *out, size_t outsz);

/* inf_hf_path_match: true when PREFIX is empty or PATH begins with it.
 * Buckets ignore the server-side prefix parameter, so sub-path selection
 * is applied client-side on the listed paths. */
bool inf_hf_path_match (const char *prefix, const char *path);

/* inf_hf_link_next: extract the rel="next" URL from a raw Link header
 * block (HF tree API pagination, 1000 entries per page).  Returns the URL
 * length, 0 when there is no next page. */
size_t inf_hf_link_next (const char *headers, char *out, size_t outsz);

/* inf_hf_array_merge: append the elements of the JSON array PAGE to the
 * accumulated malloc'd array ACC ("[..." style).  Returns the new length
 * of ACC, or 0 on error (ACC unchanged). */
size_t inf_hf_array_merge (char **acc, size_t *len, size_t *cap,
                           const char *page);

/* --- JSON array helpers (for the HF tree API) -------------------------
 * inf_json_array_count: number of elements of the top-level array.
 * inf_json_array_object: copy the IDX-th element (object or scalar) of
 * the top-level array into OUT; returns the length written (0 when IDX
 * is out of range). */
size_t inf_json_array_count (const char *json);
size_t inf_json_array_object (const char *json, size_t idx,
                              char *out, size_t outsz);

/* inf_training_search_list: like inf_training_search but collects up to
 * MAX_HITS matching lines (newline-joined) into OUT; returns the number
 * of hits written.  Used by /db search. */
size_t inf_training_search_list (const char *ndjson, const char *query,
                                 char *out, size_t outsz, size_t max_hits);

/* --- One-line machine usage (info line above the prompt) ------------ */
/* inf_client_stats_line: compact single-line CPU/memory snapshot for
 * the persistent info line ("CPU 12.3 % | MEM 456/2048 Mio (22.3 %)"),
 * bounded and NUL-terminated. */
size_t inf_client_stats_line (char *out, size_t outsz);

#endif /* INFERENCE_H */
