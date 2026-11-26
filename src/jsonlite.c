/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * client-core.c -- Pure, testable core of the phase 2 `inference`
 * client (SPEC.md section 12): editor resolution, slash command
 * parsing, narration formatting, NO_COLOR handling.  No TTY, no I/O:
 * the unit tests drive this directly.
 */

#include "inference.h"

#include <ctype.h>
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

/* inf_json_unescape: decode JSON string escapes in place (\\, \", \/,
   \b, \f, \n, \r, \t, \uXXXX basic BMP -> UTF-8).  Returns the new
   length.  Used to present stored content that was escaped once more
   than the surrounding record (JSON-in-JSON). */
size_t
inf_json_unescape (char *s)
{
  char *r = s, *w = s;
  while (*r)
    {
      if (*r == '\\' && r[1] != '\0')
        {
          r++;
          switch (*r)
            {
            case 'n': *w++ = '\n'; r++; break;
            case 't': *w++ = '\t'; r++; break;
            case 'r': *w++ = '\r'; r++; break;
            case 'b': *w++ = '\b'; r++; break;
            case 'f': *w++ = '\f'; r++; break;
            case '\\': *w++ = '\\'; r++; break;
            case '"': *w++ = '"'; r++; break;
            case '/': *w++ = '/'; r++; break;
            case 'u':
              {
                unsigned cp = 0;
                int ok = 1;
                for (int i = 1; i <= 4 && r[i] != '\0'; i++)
                  {
                    char c = r[i];
                    cp <<= 4;
                    if (c >= '0' && c <= '9')
                      cp |= (unsigned) (c - '0');
                    else if (c >= 'a' && c <= 'f')
                      cp |= (unsigned) (c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F')
                      cp |= (unsigned) (c - 'A' + 10);
                    else
                      { ok = 0; break; }
                  }
                if (ok && cp != 0)
                  {
                    r += 5;
                    if (cp < 0x80)
                      *w++ = (char) cp;
                    else if (cp < 0x800)
                      {
                        *w++ = (char) (0xC0 | (cp >> 6));
                        *w++ = (char) (0x80 | (cp & 0x3F));
                      }
                    else
                      {
                        *w++ = (char) (0xE0 | (cp >> 12));
                        *w++ = (char) (0x80 | ((cp >> 6) & 0x3F));
                        *w++ = (char) (0x80 | (cp & 0x3F));
                      }
                  }
                else
                  *w++ = *r++;
                break;
              }
            default:
              *w++ = *r++;
              break;
            }
        }
      else
        *w++ = *r++;
    }
  *w = '\0';
  return (size_t) (w - s);
}

/* inf_json_strings_extract: see include/inference.h.  One left-to-right
   scan: a literal followed by ':' is an object key and the string after
   the colon is kept as the value; a literal followed by ',', ']' or '}'
   is a bare array element and kept as-is; anything else means the
   fragment was cut mid-string, so the scan re-anchors on the quote it
   just consumed.  Escape pairs are copied raw, so an unterminated
   literal still yields its readable prefix. */
int
inf_json_strings_extract (const char *json, char *out, size_t outsz)
{
  if (json == NULL || out == NULL || outsz == 0)
    return -1;
  size_t w = 0;
  int kept = 0;
  const char *p = json;
  while (*p != '\0' && w + 2 < outsz)
    {
      if (*p == '\\' && p[1] == '"')
        {
          p += 2; /* escaped quote: not a literal opener */
          continue;
        }
      if (*p != '"')
        {
          p++;
          continue;
        }
      const char *open = p;
      p++;
      char lit[8192];
      size_t n = 0;
      while (*p != '\0' && n + 2 < sizeof lit)
        {
          if (*p == '\\' && p[1] != '\0')
            {
              lit[n++] = *p++;
              lit[n++] = *p++;
              continue;
            }
          if (*p == '"')
            break;
          lit[n++] = *p++;
        }
      int closed = (*p == '"');
      if (closed)
        p++;
      lit[n] = '\0';
      const char *q = p;
      while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
        q++;
      if (*q == ':')
        {
          /* lit is an object key: keep the string value after the colon */
          q++;
          while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
            q++;
          if (*q != '"')
            continue; /* number, bool, null or nested structure */
          p = q + 1;
          n = 0;
          while (*p != '\0' && n + 2 < sizeof lit)
            {
              if (*p == '\\' && p[1] != '\0')
                {
                  lit[n++] = *p++;
                  lit[n++] = *p++;
                  continue;
                }
              if (*p == '"')
                {
                  p++;
                  break;
                }
              lit[n++] = *p++;
            }
          lit[n] = '\0';
        }
      else if (closed && *q != ',' && *q != ']' && *q != '}')
        {
          /* fragment cut mid-string: re-anchor so the consumed quote can
             open the real literal */
          p = open + 1;
          continue;
        }
      inf_json_unescape (lit); /* one level: str_field already decoded the
                                  record; a second pass would turn the \\n
                                  of source code into a real newline */
      char *s = lit;
      while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
      size_t l = strlen (s);
      while (l > 0 && (s[l - 1] == ' ' || s[l - 1] == '\t' || s[l - 1] == '\n'
                       || s[l - 1] == '\r'))
        s[--l] = '\0';
      int alnum = 0;
      for (const char *c = s; *c != '\0'; c++)
        if (isalnum ((unsigned char) *c))
          {
            alnum = 1;
            break;
          }
      if (!alnum)
        continue;
      size_t sl = strlen (s);
      if (w + sl + 1 >= outsz)
        break;
      memcpy (out + w, s, sl);
      w += sl;
      out[w++] = '\n';
      kept++;
    }
  out[w] = '\0';
  return kept > 0 ? 0 : -1;
}
