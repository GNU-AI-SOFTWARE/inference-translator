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
const char *
inf_client_editor (void)
{
    /* Frozen order (SPEC.md section 5): $VISUAL -> $EDITOR -> nano
     * -> vi.  The last two are probed in PATH so a missing nano falls
     * back to vi without a shell. */
    const char *env;
    if ((env = getenv ("VISUAL")) != NULL && *env != '\0')
        return env;
    if ((env = getenv ("EDITOR")) != NULL && *env != '\0')
        return env;

    static const char *const fallbacks[] = { "nano", "vi" };
    const char *path = getenv ("PATH");
    char buf[4096];
    if (path == NULL)
        path = "/usr/bin:/bin";
    for (size_t i = 0; i < sizeof fallbacks / sizeof fallbacks[0]; i++)
      {
        const char *p = path;
        while (*p != '\0')
          {
            const char *colon = strchr (p, ':');
            size_t dlen = colon != NULL ? (size_t) (colon - p) : strlen (p);
            if (dlen > 0 && dlen + 1 + strlen (fallbacks[i]) < sizeof buf)
              {
                memcpy (buf, p, dlen);
                buf[dlen] = '/';
                strcpy (buf + dlen + 1, fallbacks[i]);
                if (access (buf, X_OK) == 0)
                    return fallbacks[i];
              }
            if (colon == NULL)
                break;
            p = colon + 1;
          }
      }
    return "vi";
}

enum inf_client_cmd
inf_client_parse_command (const char *line, char *arg, size_t argsz)
{
    if (line == NULL || line[0] != '/')
        return INF_CMD_NONE;

    /* Split "command [argument]" on the first space. */
    const char *sp = strchr (line, ' ');
    size_t clen = sp != NULL ? (size_t) (sp - line) : strlen (line);

    for (size_t i = 0; i < inf_actions_count; i++)
        if (strlen (inf_actions[i].command) == clen
            && strncmp (line, inf_actions[i].command, clen) == 0)
          {
            enum inf_client_cmd cmd;
            if (strcmp (inf_actions[i].command, "/editor") == 0)
                cmd = INF_CMD_EDITOR;
            else if (strcmp (inf_actions[i].command, "/task") == 0)
                cmd = INF_CMD_TASK;
            else if (strcmp (inf_actions[i].command, "/submit") == 0)
                cmd = INF_CMD_SUBMIT;
            else if (strcmp (inf_actions[i].command, "/fetch") == 0)
                cmd = INF_CMD_FETCH;
            else if (strcmp (inf_actions[i].command, "/stats") == 0)
                cmd = INF_CMD_STATS;
            else if (strcmp (inf_actions[i].command, "/status") == 0)
                cmd = INF_CMD_STATUS;
            else if (strcmp (inf_actions[i].command, "/help") == 0)
                cmd = INF_CMD_HELP;
            else if (strcmp (inf_actions[i].command, "/learn") == 0)
                cmd = INF_CMD_LEARN;
            else if (strcmp (inf_actions[i].command, "/read-code") == 0)
                cmd = INF_CMD_READ_CODE;
            else if (strcmp (inf_actions[i].command, "/learn-code") == 0)
                cmd = INF_CMD_LEARN_CODE;
            else if (strcmp (inf_actions[i].command, "/watch") == 0)
                cmd = INF_CMD_WATCH;
            else if (strcmp (inf_actions[i].command, "/history") == 0)
                cmd = INF_CMD_HISTORY;
            else if (strcmp (inf_actions[i].command, "/uptime") == 0)
                cmd = INF_CMD_UPTIME;
            else if (strcmp (inf_actions[i].command, "/reply") == 0)
                cmd = INF_CMD_REPLY;
            else if (strcmp (inf_actions[i].command, "/sigmoid") == 0)
                cmd = INF_CMD_SIGMOID;
            else if (strcmp (inf_actions[i].command, "/entrainement") == 0)
                cmd = INF_CMD_TRAINING;
            else if (strcmp (inf_actions[i].command, "/hf") == 0)
                cmd = INF_CMD_HF;
            else if (strcmp (inf_actions[i].command, "/db") == 0)
                cmd = INF_CMD_DB;
            else
                cmd = INF_CMD_QUIT;

            if ((cmd == INF_CMD_TASK || cmd == INF_CMD_FETCH
                 || cmd == INF_CMD_LEARN || cmd == INF_CMD_READ_CODE
                 || cmd == INF_CMD_LEARN_CODE || cmd == INF_CMD_TRAINING
                 || cmd == INF_CMD_REPLY || cmd == INF_CMD_HF
                 || cmd == INF_CMD_DB)
                && arg != NULL && argsz > 0)
              {
                /* No space in the line ("/task" alone): sp is NULL, the
                   argument is empty - never evaluate sp + 1. */
                const char *a = sp != NULL ? sp + 1 : "";
                while (*a == ' ')
                    a++;
                size_t alen = strlen (a);
                while (alen > 0 && (a[alen - 1] == ' ' || a[alen - 1] == '\n'
                                    || a[alen - 1] == '\r'))
                    alen--;
                if (alen >= argsz)
                    alen = argsz - 1;
                memcpy (arg, a, alen);
                arg[alen] = '\0';
              }
            return cmd;
          }
    return INF_CMD_UNKNOWN;
}

size_t
inf_client_complete (const char *line, size_t len, char *out,
                     size_t outsz, const char **list, size_t list_max)
{
    const char *cands[16];
    size_t ncand = 0;
    size_t prefix = 0;
    const char *token;
    size_t toklen;

    if (len >= 6 && strncmp (line, "/task ", 6) == 0)
      {
        prefix = 6;
        token = line + 6;
        toklen = len - 6;
        for (int t = 0; t <= INF_TASK_CUSTOM; t++)
            cands[ncand++] = inf_task_name ((enum inf_task) t);
      }
    else if (len > 0 && line[0] == '/')
      {
        token = line;
        toklen = len;
        for (size_t i = 0; i < inf_actions_count && ncand < 16; i++)
            cands[ncand++] = inf_actions[i].command;
      }
    else
        return 0;

    size_t nmatch = 0;
    const char *single = NULL;
    for (size_t i = 0; i < ncand; i++)
      {
        if (strlen (cands[i]) >= toklen
            && strncmp (cands[i], token, toklen) == 0)
          {
            if (list != NULL && nmatch < list_max)
                list[nmatch] = cands[i];
            if (nmatch == 0)
                single = cands[i];
            nmatch++;
          }
      }
    if (nmatch == 1 && out != NULL && outsz > 0)
      {
        size_t n = prefix < outsz - 1 ? prefix : outsz - 1;
        memcpy (out, line, n);
        size_t rl = strlen (single);
        if (n + rl >= outsz)
            rl = outsz - 1 - n;
        memcpy (out + n, single, rl);
        out[n + rl] = '\0';
      }
    return nmatch;
}

bool
inf_client_color (void)
{
    const char *nc = getenv ("NO_COLOR");
    return !(nc != NULL && *nc != '\0');
}

size_t
inf_client_narrate (char *out, size_t outsz, const char *verb,
                    const char *mount, const char *node)
{
    int n = snprintf (out, outsz, "%s %s/%s", verb, mount, node);
    if (n < 0)
        return 0;
    return (size_t) n < outsz ? (size_t) n : outsz - 1;
}
size_t
inf_client_banner (char *out, size_t outsz, const char *version)
{
    if (inf_client_color ())
        return (size_t) snprintf (out, outsz,
                                  "\033[1;36minference\033[0m (GNU AI) %s "
                                  "- contrat v%d\n",
                                  version, INF_CONTRACT_VERSION);
    return (size_t) snprintf (out, outsz, "inference (GNU AI) %s - contrat v%d\n",
                              version, INF_CONTRACT_VERSION);
}
