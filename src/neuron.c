/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * neuron.c -- Pure helpers around the neuron-translator contract
 * (neuron-translator README): the "Output:" section of a /llmN status
 * text ("  [3]: 0.123456"), the sigmoid activation view (/sigmoid) and
 * the deterministic text->vector mapping used to feed a neuron
 * (/entrainement).  No TTY, no I/O: the unit tests drive these
 * directly.
 */

#include "inference.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* inf_sigmoid: 1/(1+e^-x), with the input clamped so exp() cannot
 * overflow for any finite double. */
double
inf_sigmoid (double x)
{
    if (x > 700.0)
        return 1.0;
    if (x < -700.0)
        return 0.0;
    return 1.0 / (1.0 + exp (-x));
}

/* inf_parse_neuron_output: scan TEXT for the "Output:" section of a
 * neuron-translator status and parse each "  [i]: value" line into
 * VALS (at most MAX entries).  Returns the length written into *n. */
size_t
inf_parse_neuron_output (const char *text, double *vals, size_t max,
                         size_t *n)
{
    size_t count = 0;
    if (n != NULL)
        *n = 0;
    if (text == NULL || vals == NULL || max == 0)
        return 0;
    const char *p = strstr (text, "Output:");
    if (p == NULL)
        return 0;
    p += strlen ("Output:");
    while (*p != '\0' && count < max)
      {
        /* skip whitespace and newlines */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (*p != '[')
          {
            /* end of the Output section: next non-blank line that is
               not a "[i]:" entry */
            if (*p == '\0')
                break;
            const char *eol = strchr (p, '\n');
            size_t llen = eol != NULL ? (size_t) (eol - p) : strlen (p);
            if (strncmp (p, "  [", 3) != 0 && llen > 0)
                break;
            p = eol != NULL ? eol + 1 : p + llen;
            continue;
          }
        const char *colon = strchr (p, ':');
        if (colon == NULL)
            break;
        char *end = NULL;
        double v = strtod (colon + 1, &end);
        if (end == colon + 1)
            break;
        vals[count++] = v;
        const char *eol = strchr (p, '\n');
        p = eol != NULL ? eol + 1 : p + strlen (p);
      }
    if (n != NULL)
        *n = count;
    return count;
}

/* inf_sigmoid_render: one block visualizing the sigmoid state of the
 * N outputs of LABEL: per output, "label [i] raw=... sig=... |bar|".
 * The bar is 20 cells; colored green->red by activation when COLOR. */
size_t
inf_sigmoid_render (char *out, size_t outsz, const char *label,
                    const double *vals, size_t n, bool color)
{
    if (out == NULL || outsz == 0)
        return 0;
    size_t w = 0;
    #define NPUT(...)                                                     \
        do {                                                              \
            int m = snprintf (out + w, outsz - w, __VA_ARGS__);           \
            if (m < 0)                                                    \
                break;                                                    \
            if ((size_t) m >= outsz - w)                                  \
                m = (int) (outsz - w) - 1;                                \
            w += (size_t) m;                                              \
        } while (0)
    for (size_t i = 0; i < n; i++)
      {
        double s = inf_sigmoid (vals[i]);
        int cells = (int) (s * 20.0 + 0.5);
        if (cells > 20)
            cells = 20;
        NPUT ("%s [%zu] raw=%+.6f sig=%.4f ", label, i, vals[i], s);
        if (color)
            NPUT ("\033[%dm", s >= 0.66 ? 32 : s >= 0.33 ? 33 : 31);
        NPUT ("|");
        for (int c = 0; c < 20; c++)
            NPUT ("%c", c < cells ? '#' : '.');
        NPUT ("|");
        if (color)
            NPUT ("\033[0m");
        NPUT ("\n");
        if (w + 64 >= outsz)
            break;
      }
    #undef NPUT
    if (w >= outsz)
        w = outsz - 1;
    out[w] = '\0';
    return w;
}

/* inf_text_to_vector: deterministic input vector for the neuron
 * translator: the byte histogram of TEXT over N buckets, each
 * normalized by the total byte count, so every value lies in [0,1]. */
size_t
inf_text_to_vector (const char *text, double *out, size_t n)
{
    if (out == NULL || n == 0)
        return 0;
    if (text == NULL)
        text = "";
    unsigned long long buckets[256];
    memset (buckets, 0, sizeof buckets);
    unsigned long long total = 0;
    for (const unsigned char *p = (const unsigned char *) text; *p != '\0';
         p++)
      {
        buckets[*p]++;
        total++;
      }
    for (size_t i = 0; i < n; i++)
      {
        /* spread the 256 byte buckets evenly over the n outputs */
        unsigned long long acc = 0;
        size_t lo = i * 256 / n;
        size_t hi = (i + 1) * 256 / n;
        if (hi <= lo)
            hi = lo + 1;
        for (size_t b = lo; b < hi && b < 256; b++)
            acc += buckets[b];
        out[i] = total > 0 ? (double) acc / (double) total : 0.0;
      }
    return n;
}

/* inf_vector_command: render the "input v1,...,vN" write command for the
 * neuron translator from a vector (comma-separated %.6f), bounded and
 * NUL-terminated; returns the length written. */
size_t
inf_vector_command (char *out, size_t outsz, const double *vec, size_t n)
{
    if (out == NULL || outsz == 0)
        return 0;
    size_t w = 0;
    int m = snprintf (out, outsz, "input ");
    if (m < 0)
        return 0;
    w = (size_t) m < outsz ? (size_t) m : outsz - 1;
    for (size_t i = 0; i < n && w + 1 < outsz; i++)
      {
        m = snprintf (out + w, outsz - w, "%s%.6f", i > 0 ? "," : "",
                      vec != NULL ? vec[i] : 0.0);
        if (m < 0)
            break;
        if ((size_t) m >= outsz - w)
            m = (int) (outsz - w) - 1;
        w += (size_t) m;
      }
    return w;
}
