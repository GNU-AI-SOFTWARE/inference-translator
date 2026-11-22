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
/* --- SHA-256 (pure C, no external dependency) ----------------------- */

static const uint32_t sha_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
    0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
    0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
    0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
    0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
    0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define SHA_ROTR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

void
inf_sha256_hex (char out[65], const unsigned char *data, size_t len)
{
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    size_t total = ((len + 8) / 64 + 1) * 64;
    unsigned char *msg = calloc (total, 1);
    if (msg == NULL)
      {
        out[0] = '\0';
        return;
      }
    memcpy (msg, data, len);
    msg[len] = 0x80;
    uint64_t bits = (uint64_t) len * 8;
    for (int i = 0; i < 8; i++)
        msg[total - 1 - i] = (unsigned char) (bits >> (8 * i));
    for (size_t off = 0; off < total; off += 64)
      {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t) msg[off + 4 * i] << 24)
                 | ((uint32_t) msg[off + 4 * i + 1] << 16)
                 | ((uint32_t) msg[off + 4 * i + 2] << 8)
                 | (uint32_t) msg[off + 4 * i + 3];
        for (int i = 16; i < 64; i++)
          {
            uint32_t s0 = SHA_ROTR (w[i-15], 7) ^ SHA_ROTR (w[i-15], 18)
                          ^ (w[i-15] >> 3);
            uint32_t s1 = SHA_ROTR (w[i-2], 17) ^ SHA_ROTR (w[i-2], 19)
                          ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
          }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3],
                 e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++)
          {
            uint32_t S1 = SHA_ROTR (e, 6) ^ SHA_ROTR (e, 11) ^ SHA_ROTR (e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + sha_k[i] + w[i];
            uint32_t S0 = SHA_ROTR (a, 2) ^ SHA_ROTR (a, 13) ^ SHA_ROTR (a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
          }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
      }
    free (msg);
    static const char hexdig[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++)
      {
        out[8*i+0] = hexdig[(h[i] >> 28) & 15];
        out[8*i+1] = hexdig[(h[i] >> 24) & 15];
        out[8*i+2] = hexdig[(h[i] >> 20) & 15];
        out[8*i+3] = hexdig[(h[i] >> 16) & 15];
        out[8*i+4] = hexdig[(h[i] >> 12) & 15];
        out[8*i+5] = hexdig[(h[i] >> 8) & 15];
        out[8*i+6] = hexdig[(h[i] >> 4) & 15];
        out[8*i+7] = hexdig[h[i] & 15];
      }
    out[64] = '\0';
}
/* --- training element (schema.sql training_data) --------------------- */

size_t
inf_training_record (char *out, size_t outsz, const char *url,
                     long http_status, const char *text,
                     const char *checksum_hex)
{
    char eu[8192], et[65536];
    inf_json_escape (url, eu, sizeof eu);
    inf_json_escape (text, et, sizeof et);
    int n = snprintf (out, outsz,
        "{\"table\":\"training_data\",\"row\":{\"source_url\":\"%s\","
        "\"http_status\":%ld,\"content\":\"%s\",\"checksum\":\"%s\"}}",
        eu, http_status, et, checksum_hex);
    return n < 0 ? 0 : (size_t) n;
}

/* --- /reply: keyword search over the NDJSON training cache ----------- */

/* count distinct query words (lowercased, len>=3, alpha only) found in
   TEXT (case-insensitive substring search) */
static size_t
training_score (const char *text, const char *query)
{
    if (text == NULL || query == NULL)
        return 0;
    size_t score = 0;
    char word[64];
    const char *p = query;
    while (*p != '\0')
      {
        while (*p != '\0' && !((*p >= 'a' && *p <= 'z')
                               || (*p >= 'A' && *p <= 'Z')
                               || (unsigned char) *p >= 0x80))
            p++;
        size_t wl = 0;
        while (*p != '\0' && wl + 1 < sizeof word
               && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
                   || (unsigned char) *p >= 0x80))
            word[wl++] = (*p >= 'A' && *p <= 'Z') ? *p + 32 : *p, p++;
        word[wl] = '\0';
        if (wl < 3)
            continue;
        /* lowercase TEXT scan */
        size_t tl = strlen (text);
        char *low = malloc (tl + 1);
        if (low == NULL)
            continue;
        for (size_t i = 0; i < tl; i++)
            low[i] = (text[i] >= 'A' && text[i] <= 'Z') ? text[i] + 32
                     : text[i];
        low[tl] = '\0';
        if (strstr (low, word) != NULL)
            score++;
        free (low);
      }
    return score;
}

size_t
inf_training_search (const char *ndjson, const char *query,
                     char *out, size_t outsz)
{
    if (out == NULL || outsz == 0)
        return 0;
    out[0] = '\0';
    if (ndjson == NULL || query == NULL || *query == '\0')
        return 0;
    size_t best = 0;
    char bestline[300000];
    bestline[0] = '\0';
    const char *p = ndjson;
    while (*p != '\0')
      {
        const char *eol = strchr (p, '\n');
        size_t llen = eol != NULL ? (size_t) (eol - p) : strlen (p);
        if (llen > 0 && llen < sizeof bestline)
          {
            char line[300000];
            memcpy (line, p, llen);
            line[llen] = '\0';
            char content[262144];
            if (inf_json_str_field (line, "content", content,
                                    sizeof content) == 0)
              {
                size_t s = training_score (content, query);
                if (s > best)
                  {
                    best = s;
                    memcpy (bestline, line, llen + 1);
                  }
              }
          }
        p = eol != NULL ? eol + 1 : p + llen;
      }
    if (best == 0)
        return 0;
    size_t l = strlen (bestline);
    if (l >= outsz)
        l = outsz - 1;
    memcpy (out, bestline, l);
    out[l] = '\0';
    return l;
}
