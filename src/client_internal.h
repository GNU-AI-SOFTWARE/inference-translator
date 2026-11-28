/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */
/*
 * client_internal.h -- shared state and cross-module prototypes of the
 * `inference` client (client.c = REPL/process, nodeio.c = mount-point
 * I/O, cmds.c = slash commands).  Not part of the public inference.h.
 */
#ifndef INF_CLIENT_INTERNAL_H
#define INF_CLIENT_INTERNAL_H

#include "inference.h"
#include <termios.h>
#include <time.h>

extern struct termios g_saved_tio;
extern bool g_tio_saved;
extern const char *g_mount;
extern const char *g_db;
extern const char *g_orch;
extern const char *g_training_cache;
extern struct inf_history g_hist;
extern char g_hist_path[4096];
extern char g_hist_saved[INF_PROMPT_MAX];
extern size_t g_hpos;
extern time_t g_start;
extern char g_buf[INF_PROMPT_MAX];
extern size_t g_blen;

/* client.c (REPL / process) */
void restore_tio (void);
void on_signal (int sig);
bool read_line (char *line, size_t linesz, size_t *outlen, bool *escaped);

/* nodeio.c (mount-point I/O) */
int write_node_m (const char *mount, const char *node, const char *data, size_t len);
int write_node (const char *node, const char *data, size_t len);
ssize_t read_node_m (const char *mount, const char *node, char *out, size_t outsz);
ssize_t read_node (const char *node, char *out, size_t outsz);
void narrate (const char *verb, const char *node);
const char *state_ansi (const char *state);
void print_status_node (void);
void cmd_status (void);
void narrate_orch (const char *verb, const char *node);
ssize_t orch_read (const char *node, char *out, size_t outsz);
int write_root (const char *mount, const char *data, size_t len);
int write_root_line (const char *mount, const char *data, size_t len);
ssize_t read_root (const char *mount, char *out, size_t outsz);

/* cmds.c (slash commands) */
void cmd_watch (void);
void cmd_history (void);
void cmd_uptime (void);
void cmd_fetch (const char *arg);
void cmd_learn (const char *arg);
void cmd_stats (void);
void cmd_help (void);
void cmd_task (const char *arg);
void cmd_submit (void);
void cmd_editor (void);
void cmd_read_code (const char *arg);
void cmd_learn_code (const char *arg);
void cmd_reply (const char *arg);
void cmd_feedback (const char *rating);
void cmd_sigmoid (void);
void cmd_training (const char *arg);
void cmd_hf (const char *arg);
void cmd_db (const char *arg);
void cmd_code (const char *arg);
void db_store_and_report (const char *rec);

/* Append one training record line to the local NDJSON cache used by
 * /reply (best effort; returns 0 on success). */
int training_cache_append (const char *rec);

#endif /* INF_CLIENT_INTERNAL_H */
