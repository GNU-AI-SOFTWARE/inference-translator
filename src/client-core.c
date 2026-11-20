/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * client-core.c -- Pure, testable core of the phase 2 `inference`
 * client (SPEC.md section 12): editor resolution, slash command
 * parsing, narration formatting, NO_COLOR handling.  No TTY, no I/O:
 * the unit tests drive this directly.
 */

#include "inference.h"

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
            else if (strcmp (inf_actions[i].command, "/status") == 0)
                cmd = INF_CMD_STATUS;
            else
                cmd = INF_CMD_QUIT;

            if (cmd == INF_CMD_TASK && arg != NULL && argsz > 0)
              {
                const char *a = sp + 1;
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
