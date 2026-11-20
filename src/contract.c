/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * contract.c -- Pure C23/POSIX core of the phase 0 contracts.
 *
 * inf_parse_prompt () extracts the URLs of a raw prompt with the
 * frozen POSIX ERE (trailing punctuation trimmed) and detects the
 * task with the frozen keyword table inf_task_rules[].  No heap
 * allocation: the request owns fixed-size arrays and the JSON writer
 * is a bounded sink over a caller-provided buffer.
 *
 * inf_request_to_json () renders the type 1 request byte-exact:
 *   {"prompt":"...","task":"...","urls":[...],"params":{}}
 * which is what the orchestrator reads back from /inference/request.
 */

#include <regex.h>
#include <stdio.h>
#include <string.h>

#include "inference.h"

const char *
inf_task_name (enum inf_task task)
{
    switch (task)
      {
      case INF_TASK_SUMMARIZE: return "summarize";
      case INF_TASK_TRANSLATE: return "translate";
      case INF_TASK_ANSWER:    return "answer";
      case INF_TASK_CLASSIFY:  return "classify";
      case INF_TASK_CUSTOM:    return "custom";
      }
    return "custom";
}

const char *
inf_status_name (enum inf_status status)
{
    switch (status)
      {
      case INF_STATUS_IDLE:      return "idle";
      case INF_STATUS_COMPOSING: return "composing";
      case INF_STATUS_PARSED:    return "parsed";
      case INF_STATUS_SUBMITTED: return "submitted";
      }
    return "idle";
}

/* Bounded output sink: every write keeps room for the final NUL. */
struct sink
{
    char *p;
    size_t left;
};

static void
sink_put (struct sink *s, const char *text)
{
    while (*text != '\0' && s->left > 1)
      {
        *s->p++ = *text++;
        s->left--;
      }
}

static void
sink_putc (struct sink *s, char c)
{
    if (s->left > 1)
      {
        *s->p++ = c;
        s->left--;
      }
}

/* JSON-escape one UTF-8 string into the sink: quotes, backslash and
 * the short escapes for the usual controls, \u00XX for the other
 * control chars; bytes >= 0x80 (UTF-8) pass through unchanged. */
static void
sink_put_escaped (struct sink *s, const char *in)
{
    while (*in != '\0' && s->left > 1)
      {
        unsigned char c = (unsigned char) *in++;
        switch (c)
          {
          case '"':  sink_put (s, "\\\""); break;
          case '\\': sink_put (s, "\\\\"); break;
          case '\n': sink_put (s, "\\n");  break;
          case '\r': sink_put (s, "\\r");  break;
          case '\t': sink_put (s, "\\t");  break;
          case '\b': sink_put (s, "\\b");  break;
          case '\f': sink_put (s, "\\f");  break;
          default:
            if (c < 0x20)
              {
                char buf[8];
                snprintf (buf, sizeof buf, "\\u%04x", (unsigned) c);
                sink_put (s, buf);
              }
            else
              sink_putc (s, (char) c);
            break;
          }
      }
}

void
inf_json_escape (const char *in, char *out, size_t outsz)
{
    struct sink s = { out, outsz };
    sink_put_escaped (&s, in);
    *s.p = '\0';
}

/* First keyword rule that matches the prompt wins; default custom. */
static enum inf_task
detect_task (const char *prompt)
{
    for (size_t i = 0; i < inf_task_rules_count; i++)
      {
        regex_t re;
        if (regcomp (&re, inf_task_rules[i].pattern,
                     REG_EXTENDED | REG_ICASE | REG_NOSUB) != 0)
          continue;
        int found = regexec (&re, prompt, 0, NULL, 0) == 0;
        regfree (&re);
        if (found)
          return inf_task_rules[i].task;
      }
    return INF_TASK_CUSTOM;
}

size_t
inf_parse_prompt (const char *prompt, struct inf_request *req)
{
    req->prompt = prompt;
    req->url_count = 0;
    req->task = detect_task (prompt);

    regex_t re;
    if (regcomp (&re, INF_URL_REGEX, REG_EXTENDED) != 0)
      return 0;

    /* Scan the prompt forward without copying it: regexec () runs on
     * prompt + offset and reports offsets relative to that pointer. */
    size_t offset = 0;
    regmatch_t m;
    while (req->url_count < INF_URL_MAX
           && regexec (&re, prompt + offset, 1, &m, 0) == 0)
      {
        size_t start = offset + (size_t) m.rm_so;
        size_t end = offset + (size_t) m.rm_eo;
        /* Trim the trailing punctuation of the sentence, not of the
         * URL itself: , . ; : ! ? end a clause, never a host. */
        while (end > start && strchr (INF_URL_TRIM, prompt[end - 1]) != NULL)
          end--;
        size_t len = end - start;
        if (len >= INF_URL_LEN)
          len = INF_URL_LEN - 1;
        memcpy (req->urls[req->url_count], prompt + start, len);
        req->urls[req->url_count][len] = '\0';
        req->url_count++;
        offset = (m.rm_eo > 0) ? offset + (size_t) m.rm_eo : offset + 1;
      }

    regfree (&re);
    return req->url_count;
}

void
inf_request_to_json (const struct inf_request *req, char *out, size_t outsz)
{
    struct sink s = { out, outsz };

    sink_put (&s, "{\"prompt\":\"");
    sink_put_escaped (&s, req->prompt != NULL ? req->prompt : "");
    sink_put (&s, "\",\"task\":\"");
    sink_put (&s, inf_task_name (req->task));
    sink_put (&s, "\",\"urls\":[");
    for (size_t i = 0; i < req->url_count; i++)
      {
        if (i > 0)
          sink_putc (&s, ',');
        sink_putc (&s, '"');
        sink_put_escaped (&s, req->urls[i]);
        sink_putc (&s, '"');
      }
    sink_put (&s, "],\"params\":{}}");
    *s.p = '\0';
}
