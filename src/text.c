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
/* --- HTML to text (light, for training excerpts) --------------------- */

size_t
inf_html_to_text (char *out, size_t outsz, const char *html)
{
    size_t n = 0;
    bool skip = false;
    bool ws = false;
    if (outsz == 0)
        return 0;
    for (size_t i = 0; html[i] != '\0' && n + 8 < outsz;)
      {
        if (html[i] == '<')
          {
            if (strncasecmp (html + i, "<script", 7) == 0
                || strncasecmp (html + i, "<style", 6) == 0)
                skip = true;
            if (strncasecmp (html + i, "</script", 8) == 0
                || strncasecmp (html + i, "</style", 7) == 0)
                skip = false;
            size_t j = i + 1;
            while (html[j] != '\0' && html[j] != '>')
                j++;
            if (html[j] == '>')
                j++;
            if (strncasecmp (html + i, "<br", 3) == 0
                || strncasecmp (html + i, "</p", 3) == 0
                || strncasecmp (html + i, "</div", 5) == 0
                || strncasecmp (html + i, "</li", 4) == 0
                || strncasecmp (html + i, "</h", 3) == 0)
                ws = true;
            i = j;
            continue;
          }
        if (html[i] == '&')
          {
            const char *semi = strchr (html + i, ';');
            if (semi != NULL && semi - (html + i) <= 10)
              {
                char ent[12];
                size_t el = (size_t) (semi - (html + i)) - 1;
                if (el < sizeof ent)
                  {
                    memcpy (ent, html + i + 1, el);
                    ent[el] = '\0';
                    const char *rep = NULL;
                    if (strcmp (ent, "amp") == 0) rep = "&";
                    else if (strcmp (ent, "lt") == 0) rep = "<";
                    else if (strcmp (ent, "gt") == 0) rep = ">";
                    else if (strcmp (ent, "quot") == 0) rep = "\"";
                    else if (strcmp (ent, "apos") == 0
                             || strcmp (ent, "#39") == 0) rep = "'";
                    else if (strcmp (ent, "nbsp") == 0) rep = " ";
                    if (rep != NULL)
                      {
                        if (!skip)
                          {
                            if (ws && *rep != ' ' && n + 1 < outsz)
                              {
                                out[n++] = ' ';
                                ws = false;
                              }
                            for (const char *r = rep; *r && n + 1 < outsz; r++)
                                out[n++] = *r;
                          }
                        i = (size_t) (semi - html) + 1;
                        continue;
                      }
                  }
              }
          }
        if (skip)
          {
            i++;
            continue;
          }
        char c = html[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
          {
            ws = true;
            i++;
            continue;
          }
        if (ws && n > 0 && n + 1 < outsz)
            out[n++] = ' ';
        ws = false;
        out[n++] = c;
        i++;
      }
    out[n] = '\0';
    return n;
}
size_t
inf_url_trim (char *url)
{
    if (url == NULL)
        return 0;
    size_t l = strlen (url);
    while (l > 0 && (url[l - 1] == ' ' || url[l - 1] == '\t'
                     || url[l - 1] == '\r' || url[l - 1] == '\n'
                     || strchr (",;:.!?)]}'\"", url[l - 1]) != NULL))
        url[--l] = '\0';
    return l;
}
/* --- /read-code: language detection + code highlighting ------------- */

static const struct
{
    const char *ext;
    const char *lang;
} lang_exts[] = {
    { ".c", "c" }, { ".h", "c" }, { ".cc", "cpp" }, { ".cpp", "cpp" },
    { ".hpp", "cpp" }, { ".py", "python" }, { ".sh", "shell" },
    { ".bash", "shell" }, { ".js", "javascript" }, { ".mjs", "javascript" },
    { ".ts", "typescript" }, { ".rs", "rust" }, { ".go", "go" },
    { ".java", "java" }, { ".kt", "kotlin" }, { ".rb", "ruby" },
    { ".pl", "perl" }, { ".lua", "lua" }, { ".sql", "sql" },
    { ".html", "html" }, { ".htm", "html" }, { ".xml", "xml" },
    { ".css", "css" }, { ".md", "markdown" }, { ".yml", "yaml" },
    { ".yaml", "yaml" }, { ".json", "json" }, { ".toml", "toml" },
    { ".am", "make" }, { ".ac", "autoconf" }, { ".m4", "autoconf" },
    { ".defs", "mig" }, { ".texi", "texinfo" }, { ".1", "man" },
    { NULL, NULL }
};

const char *
inf_lang_detect (const char *url, const char *content)
{
    /* 1) extension of the URL path (query/fragment stripped) */
    if (url != NULL)
      {
        const char *path = strstr (url, "://");
        path = path != NULL ? strchr (path + 3, '/') : NULL;
        const char *base = path != NULL ? path : url;
        const char *q = strpbrk (base, "?#");
        char extbuf[16];
        size_t bl = q != NULL ? (size_t) (q - base) : strlen (base);
        const char *dot = NULL;
        for (size_t i = 0; i < bl; i++)
            if (base[i] == '.')
                dot = base + i;
        if (dot != NULL && q == NULL ? true : (q != NULL && dot < q))
          {
            size_t el = q != NULL ? (size_t) (q - dot) : strlen (dot);
            if (el > 1 && el < sizeof extbuf)
              {
                memcpy (extbuf, dot, el);
                extbuf[el] = '\0';
                for (size_t i = 0; lang_exts[i].ext != NULL; i++)
                    if (strcasecmp (extbuf, lang_exts[i].ext) == 0)
                        return lang_exts[i].lang;
              }
          }
      }
    /* 2) shebang */
    if (content != NULL && content[0] == '#' && content[1] == '!')
      {
        if (strstr (content, "python") != NULL)
            return "python";
        if (strstr (content, "sh") != NULL || strstr (content, "bash") != NULL)
            return "shell";
        if (strstr (content, "perl") != NULL)
            return "perl";
        if (strstr (content, "ruby") != NULL)
            return "ruby";
        if (strstr (content, "node") != NULL)
            return "javascript";
      }
    return "text";
}

/* generic keyword set for c-like, python and shell */
static const char *const code_keywords[] = {
    "if", "else", "for", "while", "do", "switch", "case", "default",
    "break", "continue", "return", "goto", "typedef", "struct",
    "union", "enum", "static", "const", "extern", "volatile", "inline",
    "include", "define", "ifdef", "ifndef", "endif", "function",
    "def", "class", "import", "from", "as", "lambda", "with", "try",
    "except", "finally", "raise", "pass", "print", "None", "True",
    "False", "let", "var", "fn", "pub", "impl", "match", "package",
    "func", "go", "chan", "select", "local", "then", "fi", "esac",
    "in", "echo", "export", "source", NULL
};

static bool
is_word_char (char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
           || (c >= '0' && c <= '9') || c == '_';
}

/* Render source code: comments dim, strings green, numbers magenta,
 * keywords bold blue.  Comment styles: "#..." for python/shell/yaml/
 * make/autoconf/mig, line "//" and block slash-star comments for
 * c-like, "<!--...-->" for html/xml, "--" for sql. */
size_t
inf_code_highlight (char *out, size_t outsz, const char *code,
                    const char *lang, bool color)
{
    size_t n = 0;
    if (outsz == 0)
        return 0;
    if (!color)
      {
        size_t l = strlen (code);
        if (l > outsz - 1)
            l = outsz - 1;
        memcpy (out, code, l);
        out[l] = '\0';
        return l;
      }
    bool hash_comment = lang != NULL
        && (strcmp (lang, "python") == 0 || strcmp (lang, "shell") == 0
            || strcmp (lang, "yaml") == 0 || strcmp (lang, "make") == 0
            || strcmp (lang, "autoconf") == 0 || strcmp (lang, "toml") == 0);
    bool sql_comment = lang != NULL && strcmp (lang, "sql") == 0;
    bool markup = lang != NULL
        && (strcmp (lang, "html") == 0 || strcmp (lang, "xml") == 0
            || strcmp (lang, "markdown") == 0);
    bool c_like = !hash_comment && !sql_comment && !markup;

#define CODE_PUT(str)                                                   \
    do {                                                                \
        size_t cl_ = strlen (str);                                      \
        if (n + cl_ < outsz)                                            \
          {                                                             \
            memcpy (out + n, str, cl_);                                 \
            n += cl_;                                                   \
          }                                                             \
    } while (0)

    for (size_t i = 0; code[i] != '\0' && n + 24 < outsz;)
      {
        /* comments */
        if ((hash_comment && code[i] == '#')
            || (sql_comment && code[i] == '-' && code[i + 1] == '-')
            || (c_like && code[i] == '/' && code[i + 1] == '/')
            || (c_like && code[i] == '/' && code[i + 1] == '*')
            || (markup && code[i] == '<' && strncmp (code + i, "<!--", 4) == 0))
          {
            size_t j = i;
            if (code[i] == '#' || (sql_comment && code[i] == '-'))
                while (code[j] != '\0' && code[j] != '\n')
                    j++;
            else if (c_like && code[i + 1] == '/')
                while (code[j] != '\0' && code[j] != '\n')
                    j++;
            else if (c_like && code[i + 1] == '*')
              {
                j = i + 2;
                while (code[j] != '\0'
                       && !(code[j] == '*' && code[j + 1] == '/'))
                    j++;
                if (code[j] == '*' && code[j + 1] == '/')
                    j += 2;
              }
            else /* markup <!-- ... --> */
              {
                j = i + 4;
                while (code[j] != '\0'
                       && strncmp (code + j, "-->", 3) != 0)
                    j++;
                if (strncmp (code + j, "-->", 3) == 0)
                    j += 3;
              }
            CODE_PUT ("\033[90m");
            if (n + (j - i) < outsz - 1)
              {
                memcpy (out + n, code + i, j - i);
                n += j - i;
              }
            CODE_PUT ("\033[0m");
            i = j;
            continue;
          }
        /* strings */
        if (code[i] == '"' || code[i] == '\'')
          {
            char q = code[i];
            size_t j = i + 1;
            while (code[j] != '\0' && code[j] != q)
                j += (code[j] == '\\' && code[j + 1] != '\0') ? 2 : 1;
            if (code[j] == q)
                j++;
            CODE_PUT ("\033[32m");
            if (n + (j - i) < outsz - 1)
              {
                memcpy (out + n, code + i, j - i);
                n += j - i;
              }
            CODE_PUT ("\033[0m");
            i = j;
            continue;
          }
        /* numbers */
        if ((code[i] >= '0' && code[i] <= '9')
            && (i == 0 || !is_word_char (code[i - 1])))
          {
            size_t j = i;
            while (is_word_char (code[j]) || code[j] == '.' || code[j] == 'x'
                   || code[j] == 'X')
                j++;
            CODE_PUT ("\033[35m");
            if (n + (j - i) < outsz - 1)
              {
                memcpy (out + n, code + i, j - i);
                n += j - i;
              }
            CODE_PUT ("\033[0m");
            i = j;
            continue;
          }
        /* keywords */
        if ((code[i] >= 'a' && code[i] <= 'z')
            || (code[i] >= 'A' && code[i] <= 'Z') || code[i] == '_')
          {
            size_t j = i;
            while (is_word_char (code[j]))
                j++;
            size_t wl = j - i;
            bool kw = false;
            for (size_t k = 0; code_keywords[k] != NULL && !kw; k++)
                if (strlen (code_keywords[k]) == wl
                    && strncmp (code + i, code_keywords[k], wl) == 0)
                    kw = true;
            if (kw)
              {
                CODE_PUT ("\033[1;34m");
                if (n + wl < outsz - 1)
                  {
                    memcpy (out + n, code + i, wl);
                    n += wl;
                  }
                CODE_PUT ("\033[0m");
                i = j;
                continue;
              }
            if (n + wl < outsz - 1)
              {
                memcpy (out + n, code + i, wl);
                n += wl;
              }
            i = j;
            continue;
          }
        out[n++] = code[i++];
      }
    out[n] = '\0';
    return n;
}

#undef CODE_PUT

/* --- URL validation for /fetch -------------------------------------- */

bool
inf_url_valid (const char *url)
{
    if (url == NULL)
        return false;
    size_t off = 0;
    if (strncmp (url, "https://", 8) == 0)
        off = 8;
    else if (strncmp (url, "http://", 7) == 0)
        off = 7;
    else
        return false;
    /* host must be non-empty and not just "/" or "?" (curl would
       answer "URL using bad/illegal format or missing URL") */
    if (url[off] == '\0' || url[off] == '/' || url[off] == '?')
        return false;
    return true;
}

/* --- URL host extraction + /etc/hosts lookup (auto-DNS) --------------- */

bool
inf_hf_url (const char *url)
{
    return url != NULL && strncmp (url, "hf://", 5) == 0;
}

bool
inf_url_host (const char *url, char *host, size_t hostsz)
{
    if (url == NULL || host == NULL || hostsz == 0)
        return false;
    size_t off = 0;
    if (strncmp (url, "https://", 8) == 0)
        off = 8;
    else if (strncmp (url, "http://", 7) == 0)
        off = 7;
    else
        return false;
    size_t n = 0;
    for (size_t i = off; url[i] != '\0' && url[i] != '/' && url[i] != ':'
         && url[i] != '?' && url[i] != '#'; i++)
      {
        if (n + 1 >= hostsz)
            return false;
        host[n++] = url[i];
      }
    host[n] = '\0';
    return n > 0;
}

bool
inf_hosts_has (const char *hosts_text, const char *host)
{
    if (hosts_text == NULL || host == NULL || *host == '\0')
        return false;
    size_t hlen = strlen (host);
    const char *p = hosts_text;
    while (*p != '\0')
      {
        const char *eol = strchr (p, '\n');
        size_t llen = eol != NULL ? (size_t) (eol - p) : strlen (p);
        /* skip comments and blank lines */
        const char *q = p;
        while (q < p + llen && (*q == ' ' || *q == '\t'))
            q++;
        if (q < p + llen && *q != '#')
          {
            /* fields: first is the address, the rest are names */
            const char *f = q;
            while (f < p + llen)
              {
                while (f < p + llen && (*f == ' ' || *f == '\t'))
                    f++;
                const char *fs = f;
                while (f < p + llen && *f != ' ' && *f != '\t')
                    f++;
                size_t flen = (size_t) (f - fs);
                if (flen == hlen && strncmp (fs, host, hlen) == 0)
                    return true;
              }
          }
        p = eol != NULL ? eol + 1 : p + llen;
      }
    return false;
}

/* --- phase 3: syntax highlighting (SPEC.md section 6) --------------- */

#define HL_PUT(str)                                                     \
    do {                                                                \
        size_t hl_l_ = strlen (str);                                    \
        if (n + hl_l_ < outsz)                                          \
          {                                                             \
            memcpy (out + n, str, hl_l_);                               \
            n += hl_l_;                                                 \
          }                                                             \
    } while (0)

/* Render JSON with ANSI colors when COLOR is true (keys bold cyan,
 * strings green, numbers magenta, literals yellow, punctuation dim);
 * with color=false the input is copied verbatim. */
size_t
inf_json_highlight (char *out, size_t outsz, const char *json, bool color)
{
    size_t n = 0;
    if (outsz == 0)
        return 0;
    if (!color)
      {
        size_t l = strlen (json);
        if (l > outsz - 1)
            l = outsz - 1;
        memcpy (out, json, l);
        out[l] = '\0';
        return l;
      }
    for (size_t i = 0; json[i] != '\0' && n + 16 < outsz;)
      {
        char c = json[i];
        if (c == '"')
          {
            /* scan the whole string token */
            size_t j = i + 1;
            while (json[j] != '\0' && json[j] != '"')
                j += (json[j] == '\\' && json[j + 1] != '\0') ? 2 : 1;
            if (json[j] == '"')
                j++;
            /* a string followed by ':' is an object key */
            size_t k = j;
            while (json[k] == ' ' || json[k] == '\t')
                k++;
            bool is_key = json[k] == ':';
            HL_PUT (is_key ? "\033[1;36m" : "\033[32m");
            if (n + (j - i) < outsz - 1)
              {
                memcpy (out + n, json + i, j - i);
                n += j - i;
              }
            HL_PUT ("\033[0m");
            i = j;
            continue;
          }
        if (c == '-' || (c >= '0' && c <= '9'))
          {
            size_t j = i;
            while (json[j] == '-' || json[j] == '+' || json[j] == '.'
                   || json[j] == 'e' || json[j] == 'E'
                   || (json[j] >= '0' && json[j] <= '9'))
                j++;
            HL_PUT ("\033[35m");
            if (n + (j - i) < outsz - 1)
              {
                memcpy (out + n, json + i, j - i);
                n += j - i;
              }
            HL_PUT ("\033[0m");
            i = j;
            continue;
          }
        if (strncmp (json + i, "true", 4) == 0
            || strncmp (json + i, "false", 5) == 0
            || strncmp (json + i, "null", 4) == 0)
          {
            size_t l = strncmp (json + i, "false", 5) == 0 ? 5 : 4;
            HL_PUT ("\033[33m");
            if (n + l < outsz - 1)
              {
                memcpy (out + n, json + i, l);
                n += l;
              }
            HL_PUT ("\033[0m");
            i += l;
            continue;
          }
        if (c == '{' || c == '}' || c == '[' || c == ']' || c == ':'
            || c == ',')
          {
            HL_PUT ("\033[2m");
            out[n++] = c;
            HL_PUT ("\033[0m");
            i++;
            continue;
          }
        out[n++] = c;
        i++;
      }
    out[n] = '\0';
    return n;
}

/* Render prompt text: http(s):// URLs underlined cyan, **bold**,
 * *italic* and `code` spans highlighted; plain copy when !color. */
size_t
inf_text_highlight (char *out, size_t outsz, const char *text, bool color)
{
    size_t n = 0;
    if (outsz == 0)
        return 0;
    if (!color)
      {
        size_t l = strlen (text);
        if (l > outsz - 1)
            l = outsz - 1;
        memcpy (out, text, l);
        out[l] = '\0';
        return l;
      }
    for (size_t i = 0; text[i] != '\0' && n + 16 < outsz;)
      {
        char c = text[i];
        if ((strncmp (text + i, "http://", 7) == 0
             || strncmp (text + i, "https://", 8) == 0))
          {
            size_t l = strncmp (text + i, "https://", 8) == 0 ? 8 : 7;
            size_t j = i + l;
            while (text[j] != '\0' && text[j] != ' ' && text[j] != '\t'
                   && text[j] != '\n')
                j++;
            HL_PUT ("\033[4;36m");
            if (n + (j - i) < outsz - 1)
              {
                memcpy (out + n, text + i, j - i);
                n += j - i;
              }
            HL_PUT ("\033[0m");
            i = j;
            continue;
          }
        if (strncmp (text + i, "**", 2) == 0)
          {
            size_t j = i + 2;
            while (text[j] != '\0' && strncmp (text + j, "**", 2) != 0)
                j++;
            if (text[j] == '*' && text[j + 1] == '*')
                j += 2;
            HL_PUT ("\033[1m");
            if (n + (j - i) < outsz - 1)
              {
                memcpy (out + n, text + i, j - i);
                n += j - i;
              }
            HL_PUT ("\033[0m");
            i = j;
            continue;
          }
        if (c == '*' || c == '`')
          {
            char close = c;
            size_t j = i + 1;
            while (text[j] != '\0' && text[j] != close)
                j++;
            if (text[j] == close)
                j++;
            HL_PUT (c == '*' ? "\033[3m" : "\033[2m");
            if (n + (j - i) < outsz - 1)
              {
                memcpy (out + n, text + i, j - i);
                n += j - i;
              }
            HL_PUT ("\033[0m");
            i = j;
            continue;
          }
        out[n++] = c;
        i++;
      }
    out[n] = '\0';
    return n;
}

