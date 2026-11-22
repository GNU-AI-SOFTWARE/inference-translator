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
/* --- Phase 4 (/watch): readers for the frozen orchestrator JSON ----- */

/* Find "key" as a JSON string key, then the ':' and the value that
 * follows.  Returns a pointer just past the ':' (value start), or NULL
 * when the key is absent.  Only flat string keys are supported: the
 * match must be preceded by '{' or ',' (ignoring whitespace), so a
 * nested object re-using the same key name is not confused. */
static const char *
json_key_value (const char *json, const char *key)
{
    if (json == NULL || key == NULL)
        return NULL;
    char pat[128];
    int n = snprintf (pat, sizeof pat, "\"%s\"", key);
    if (n < 0 || (size_t) n >= sizeof pat)
        return NULL;
    const char *p = json;
    while ((p = strstr (p, pat)) != NULL)
      {
        const char *before = p;
        while (before > json && (before[-1] == ' ' || before[-1] == '\t'
                                 || before[-1] == '\n' || before[-1] == '\r'))
            before--;
        if (before > json && (before[-1] == '{' || before[-1] == ','))
          {
            const char *v = p + strlen (pat);
            while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')
                v++;
            if (*v == ':')
              {
                v++;
                while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')
                    v++;
                return v;
              }
          }
        p += strlen (pat);
      }
    return NULL;
}

int
inf_json_str_field (const char *json, const char *key,
                    char *out, size_t outsz)
{
    const char *v = json_key_value (json, key);
    if (v == NULL || *v != '"' || out == NULL || outsz == 0)
        return -1;
    v++;
    size_t n = 0;
    while (*v != '\0' && *v != '"')
      {
        char c = *v;
        if (c == '\\' && v[1] != '\0')
          {
            v++;
            c = *v;
            if (c == 'n')
                c = '\n';
            else if (c == 't')
                c = '\t';
            else if (c == 'r')
                c = '\r';
          }
        if (n + 1 < outsz)
            out[n++] = c;
        v++;
      }
    if (*v != '"')
        return -1;
    out[n] = '\0';
    return 0;
}

int
inf_json_num_field (const char *json, const char *key, double *out)
{
    const char *v = json_key_value (json, key);
    if (v == NULL || out == NULL)
        return -1;
    char *end = NULL;
    double d = strtod (v, &end);
    if (end == v)
        return -1;
    *out = d;
    return 0;
}

bool
inf_orch_role_ok (const char *node, const char *buf)
{
    if (node == NULL || buf == NULL)
        return false;
    if (strstr (buf, "\"empty\"") != NULL)
        return true;
    if (strcmp (node, "status") == 0)
        return strstr (buf, "\"instances\"") != NULL;
    return strstr (buf, "\"output\"") != NULL;
}
