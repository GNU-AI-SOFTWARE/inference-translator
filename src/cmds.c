/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * client.c -- The phase 2 `inference` client (SPEC.md section 12).
 *
 * Minimal interactive IHM: colored banner, multi-line input in raw
 * termios mode, the frozen slash commands (/editor, /task, /submit,
 * /status, /quit), $EDITOR integration on a /tmp file, and a
 * narration line naming the POSIX node involved at each step.
 *
 * The client only ever talks to the mount point through plain POSIX
 * open/read/write: default /inference, overridable with --mount or
 * $INFERENCE_MOUNT (used by the tests against a fake tree).
 *
 * No TUI library: raw termios + ANSI codes, NO_COLOR honored, and the
 * interactive mode refuses to start without a terminal (exit 2).
 */

/* The interactive mode uses POSIX cbreak termios, NOT cfmakeraw():
 * cfmakeraw() clears OPOST (hence ONLCR), and then every '\n' printed
 * by the program moves down one line without returning to column 0 -
 * the staircase display.  With OPOST|ONLCR kept, a plain '\n' is
 * post-processed to CR LF as POSIX specifies. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "inference.h"
#include "client_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef VERSION
# ifdef PACKAGE_VERSION
#  define VERSION PACKAGE_VERSION
# else
#  define VERSION "0.1.0"
# endif
#endif

/* /watch — phase 4: follow a run on the orchestrator: poll the type 2
 * status until a terminal state, then show the type 3 result
 * (highlighted JSON, output printed typewriter-style). */
void
cmd_watch (void)
{
    char status[INF_JSON_MAX];
    narrate_orch ("read", "status");
    if (orch_read ("status", status, sizeof status) < 0)
      {
        printf ("  orchestrateur injoignable: %s non monté ?\n"
                "  settrans -a %s orchestrator-translator ...\n",
                g_orch, g_orch);
        return;
      }

    static const char frames[] = "|/-\\";
    char state[64] = "";
    bool terminal = false;
    printf ("  surveillance ");
    fflush (stdout);
    for (int i = 0; i < 100 && !terminal; i++)
      {
        if (orch_read ("status", status, sizeof status) < 0)
            break;
        status[strcspn (status, "\r\n")] = '\0';
        if (inf_json_str_field (status, "state", state, sizeof state) != 0)
            break;
        terminal = strcmp (state, "done") == 0 || strcmp (state, "failed") == 0
                   || strcmp (state, "idle") == 0;
        if (!terminal)
          {
            printf ("\r  surveillance %c  state=%s ", frames[i % 4], state);
            fflush (stdout);
            usleep (200 * 1000);
          }
      }
    printf ("\r                       \r");
    fflush (stdout);

    char hl[INF_JSON_MAX * 4];
    inf_json_highlight (hl, sizeof hl, status, inf_client_color ());
    printf ("  status: %s\n", hl);
    double run_id = 0;
    (void) inf_json_num_field (status, "run_id", &run_id);
    const char *a = state_ansi (state);
    if (*a)
        printf ("  run #%.0f, état: %s%s\033[0m\n", run_id, a, state);
    else
        printf ("  run #%.0f, état: %s\n", run_id, state);
    if (strcmp (state, "idle") == 0)
      {
        printf ("  aucun run en cours: /submit d'abord, ou lancez un\n"
                "  descripteur sur %s (l'orchestrateur lira /inference/request)\n",
                g_orch);
        return;
      }

    char result[INF_JSON_MAX];
    narrate_orch ("read", "result");
    if (orch_read ("result", result, sizeof result) < 0)
      {
        printf ("  result illisible\n");
        return;
      }
    result[strcspn (result, "\r\n")] = '\0';
    if (strstr (result, "\"empty\"") != NULL)
      {
        printf ("  aucun résultat disponible ({\"empty\": true})\n");
        return;
      }
    inf_json_highlight (hl, sizeof hl, result, inf_client_color ());
    printf ("  result: %s\n", hl);

    char output[INF_JSON_MAX];
    if (inf_json_str_field (result, "output", output, sizeof output) == 0)
      {
        printf ("  output: ");
        fflush (stdout);
        size_t n = strlen (output);
        if (n > 4000)
            n = 4000;
        for (size_t i = 0; i < n; i++)
          {
            putchar (output[i]);
            fflush (stdout);
            usleep (12 * 1000);
          }
        printf ("\n");
      }
    double conf = 0.0;
    if (inf_json_num_field (result, "confidence", &conf) == 0)
      {
        const char *c = !inf_client_color () ? ""
            : conf >= 0.8 ? "\033[32m" : conf >= 0.5 ? "\033[33m" : "\033[1;31m";
        printf ("  confiance: %s%.3f\033[0m\n", c, conf);
      }
}

/* /history — list the session history (most recent last, like the
 * `history` builtin); up/down arrows recall entries at the prompt. */
void
cmd_history (void)
{
    printf ("  historique (%zu entrée%s, %s) :\n", g_hist.count,
            g_hist.count > 1 ? "s" : "", g_hist_path);
    for (size_t i = 0; i < g_hist.count; i++)
        printf ("  %4zu  %s\n", i + 1, inf_history_get (&g_hist, i));
}

/* /uptime — client version, translator version (read from
 * <mount>/version) and the session uptime. */
void
cmd_uptime (void)
{
    char up[64];
    inf_format_uptime (up, sizeof up,
                       (long) difftime (time (NULL), g_start));
    printf ("  client: inference (GNU AI) %s, uptime: %s\n", VERSION, up);
    char ver[512];
    narrate ("read", INF_NODE_VERSION);
    if (read_node (INF_NODE_VERSION, ver, sizeof ver) < 0)
        printf ("  translator: version illisible (non monté ?)\n");
    else
      {
        ver[strcspn (ver, "\r\n")] = '\0';
        printf ("  translator: %s\n", ver);
      }
}

/* --- /fetch: an HTML page through httpfs ----------------------------- */

#define FETCH_MOUNT     "/page"
#define FETCH_MAX_BODY  (4 * 1024 * 1024)   /* one HF tree page (~1000 entries) is ~200 KB */
#define FETCH_PREVIEW   4000

/* find_httpfs — locate the httpfs translator binary: $HTTPFS_BIN,
   then the usual install/build locations, then PATH (settrans will
   resolve a bare name). */
static const char *
find_httpfs (void)
{
    static char buf[4096];
    const char *env = getenv ("HTTPFS_BIN");
    if (env != NULL && env[0] != '\0' && access (env, X_OK) == 0)
        return env;
    if (access ("/usr/local/bin/httpfs", X_OK) == 0)
        return "/usr/local/bin/httpfs";
    if (access ("/root/httpfs-translator/src/httpfs", X_OK) == 0)
        return "/root/httpfs-translator/src/httpfs";
    (void) buf;
    return "httpfs";
}

/* shell_quote — copy S into DST as a single-quoted /bin/sh word. */
static void
shell_quote (char *dst, size_t dstsz, const char *s)
{
    size_t n = 0;
    if (dstsz == 0)
        return;
    dst[n++] = '\'';
    for (const char *p = s; *p != '\0' && n + 4 < dstsz; p++)
      {
        if (*p == '\'')
          {
            dst[n++] = '\\';
            dst[n++] = '\'';
            dst[n++] = '\'';
          }
        else
            dst[n++] = *p;
      }
    dst[n++] = '\'';
    dst[n] = '\0';
}

/* narrate_path — narration for an absolute path outside g_mount
   (e.g. /llm1, /page): narrate() would wrongly prefix g_mount. */
static void
narrate_path (const char *verb, const char *path)
{
    if (inf_client_color ())
        printf ("\033[2m→ %s %s\033[0m\n", verb, path);
    else
        printf ("→ %s %s\n", verb, path);
}

/* dns_ensure_host — auto-DNS: when the host of URL is not an IP
   literal and /etc/hosts does not know it, resolve it once (getent)
   and append "IP host" to /etc/hosts, so httpfs — which relies on the
   system resolver — can reach it even when the VM has no working DNS.
   Best effort: every failure is only reported, never fatal. */
static void
dns_ensure_host (const char *url)
{
    char host[256];
    if (!inf_url_host (url, host, sizeof host))
        return;
    bool is_ip = true;
    for (const char *p = host; *p != '\0'; p++)
        if (!((*p >= '0' && *p <= '9') || *p == '.'))
          {
            is_ip = false;
            break;
          }
    if (is_ip)
        return;                         /* IP literal: nothing to add */
    char hosts[65536];
    ssize_t hn = 0;
    int fd = open ("/etc/hosts", O_RDONLY);
    if (fd >= 0)
      {
        hn = read (fd, hosts, sizeof hosts - 1);
        close (fd);
        if (hn < 0)
            hn = 0;
      }
    hosts[hn] = '\0';
    if (inf_hosts_has (hosts, host))
        return;                         /* already mapped */
    char q[300];
    char cmd[600];
    shell_quote (q, sizeof q, host);
    snprintf (cmd, sizeof cmd, "getent hosts %s 2>/dev/null | head -1", q);
    FILE *f = popen (cmd, "r");
    if (f == NULL)
        return;
    char line[512];
    char ip[64] = "";
    if (fgets (line, sizeof line, f) != NULL)
        sscanf (line, "%63s", ip);
    pclose (f);
    if (ip[0] == '\0')
      {
        /* No answer: in a QEMU user-net VM the DNS forwarder is
           10.0.2.3; when /etc/resolv.conf has no nameserver at all,
           append it (root) and retry once. */
        char rc[8192];
        ssize_t rn = 0;
        int rfd = open ("/etc/resolv.conf", O_RDONLY);
        if (rfd >= 0)
          {
            rn = read (rfd, rc, sizeof rc - 1);
            close (rfd);
            if (rn < 0)
                rn = 0;
          }
        rc[rn] = '\0';
        if (strstr (rc, "nameserver") == NULL)
          {
            rfd = open ("/etc/resolv.conf", O_WRONLY | O_APPEND | O_CREAT,
                        0644);
            if (rfd >= 0)
              {
                (void) write (rfd, "nameserver 10.0.2.3\n", 20);
                close (rfd);
                printf ("  DNS: aucun nameserver dans /etc/resolv.conf — "
                        "ajout de 10.0.2.3 (DNS QEMU user-net), nouvel essai\n");
                FILE *f2 = popen (cmd, "r");
                if (f2 != NULL)
                  {
                    if (fgets (line, sizeof line, f2) != NULL)
                        sscanf (line, "%63s", ip);
                    pclose (f2);
                  }
              }
          }
      }
    if (ip[0] == '\0')
      {
        printf ("  DNS: %s introuvable (getent) — /etc/hosts inchangé\n"
                "  (VM QEMU user-net : vérifiez « nameserver 10.0.2.3 » dans "
                "/etc/resolv.conf)\n", host);
        return;
      }
    fd = open ("/etc/hosts", O_WRONLY | O_APPEND);
    if (fd < 0)
      {
        printf ("  DNS: /etc/hosts non inscriptible (%s) — relancez en root\n",
                strerror (errno));
        return;
      }
    char entry[400];
    int m = snprintf (entry, sizeof entry, "%s %s\n", ip, host);
    if (m > 0)
        (void) write (fd, entry, (size_t) m);
    close (fd);
    printf ("  DNS: %s -> %s ajouté à /etc/hosts\n", host, ip);
}

/* fetch_url_h — mount httpfs on /page for URL, wait for the HEAD probe,
 * read the body, and (when HEADERS is non-NULL) the raw probe headers
 * (Link rel="next" pagination cursor of the HF tree API).
 * Returns 0 when an HTTP status was obtained (even "000": the probe
 * failed, *out_status == 0 and *out_body == NULL), -1 when the
 * mount/probe failed.  On success *out_body is malloc'd (NULL when
 * there is no body); the caller frees it. */
static int
fetch_url_h (const char *url, long *out_status, char **out_body,
             size_t *out_len, char *headers, size_t hsz)
{
    *out_body = NULL;
    *out_len = 0;
    dns_ensure_host (url);
    char q[2048];
    char b[2048];
    char cmd[4600];
    shell_quote (q, sizeof q, url);
    shell_quote (b, sizeof b, find_httpfs ());
    /* Robust remount: kill any previous translator on FETCH_MOUNT and
       retry the activation — on Hurd the removal can lag slightly and a
       bare "settrans -a" then fails with EBUSY ("Device or resource
       busy") when two fetches follow each other. */
    snprintf (cmd, sizeof cmd,
              "i=0; while [ $i -lt 6 ]; do "
              "settrans -g -f " FETCH_MOUNT " 2>/dev/null; "
              "sleep 1; "
              "mkdir -p " FETCH_MOUNT "; "
              "if settrans -a " FETCH_MOUNT " %s %s 2>/dev/null; then exit 0; fi; "
              "i=$((i+1)); done; exit 1", b, q);
    printf ("→ mount %s %s sur " FETCH_MOUNT "\n", b, url);
    if (system (cmd) != 0)
      {
        printf ("  échec du montage (settrans + httpfs requis, root, Hurd)\n");
        return -1;
      }

    /* Wait for the HEAD probe: /page/status appears once it is done. */
    char val[64];
    long status = -1;
    for (int i = 0; i < 75; i++)            /* ~15 s */
      {
        ssize_t n = read_node_m (FETCH_MOUNT, "status", val, sizeof val);
        if (n > 0)
          {
            status = strtol (val, nullptr, 10);
            break;
          }
        usleep (200 * 1000);
      }
    if (status < 0)
      {
        printf ("  " FETCH_MOUNT "/status illisible après ~15 s\n"
                "  translator: ");
        fflush (stdout);
        system ("settrans -p " FETCH_MOUNT " 2>&1 | head -1");
        printf ("  (translator vivant mais sonde HEAD en échec -> DNS/réseau ?\n"
                "   essayez une IP directe: /fetch http://93.184.216.34)\n");
        return -1;
      }
    *out_status = status;
    if (status == 0)
        return 0;                       /* transport error in headers */

    char *body = malloc (FETCH_MAX_BODY + 1);
    if (body == nullptr)
      {
        printf ("  ENOMEM\n");
        return -1;
      }
    ssize_t total = read_node_m (FETCH_MOUNT, "content", body,
                                 FETCH_MAX_BODY + 1);
    if (total < 0)
      {
        printf ("  lecture de " FETCH_MOUNT "/content impossible\n");
        free (body);
        return -1;
      }
    *out_body = body;
    *out_len = (size_t) total;
    if (headers != NULL && hsz > 0)
      {
        ssize_t hn = read_node_m (FETCH_MOUNT, "headers", headers, hsz);
        if (hn < 0)
            headers[0] = '\0';
      }
    return 0;
}

static int
fetch_url (const char *url, long *out_status, char **out_body,
           size_t *out_len)
{
    return fetch_url_h (url, out_status, out_body, out_len, NULL, 0);
}

static void
print_http_status (long status)
{
    const char *a = "";
    if (inf_client_color ())
        a = (status >= 200 && status < 300) ? "\033[32m"
            : (status == 0 ? "\033[1;31m" : "\033[33m");
    if (*a)
        printf ("  status HTTP: %s%ld\033[0m\n", a, status);
    else
        printf ("  status HTTP: %ld\n", status);
}

void
cmd_fetch (const char *arg)
{
    char urlbuf[2048];
    if (arg == NULL || *arg == '\0')
      {
        printf ("  usage: /fetch URL (http://... ou https://...)\n");
        return;
      }
    snprintf (urlbuf, sizeof urlbuf, "%s", arg);
    inf_url_trim (urlbuf);
    arg = urlbuf;
    if (!inf_url_valid (arg))
      {
        printf ("  URL invalide: http:// ou https:// suivi d'un hôte requis\n");
        return;
      }
    long status;
    char *body;
    size_t blen;
    if (fetch_url (arg, &status, &body, &blen) != 0)
        return;
    print_http_status (status);
    if (status == 0)
      {
        /* "000": the HEAD probe failed; /page/headers holds the
           transport error text (curl) thanks to the httpfs fix. */
        char errbuf[512];
        ssize_t en = read_node_m (FETCH_MOUNT, "headers", errbuf,
                                  sizeof errbuf);
        printf ("  transport: %s",
                en > 0 ? errbuf : "erreur inconnue\n");
        return;
      }
    size_t show = blen < FETCH_PREVIEW ? blen : FETCH_PREVIEW;
    printf ("  page: %zu octets (aperçu %zu) — complète dans "
            FETCH_MOUNT "/content\n", blen, show);
    fwrite (body, 1, show, stdout);
    printf ("\n");
    free (body);
}

/* --- /learn: store fetched pages as training elements in /db -------- */


void db_store_and_report (const char *rec);

void
cmd_learn (const char *arg)
{
    /* URLs: explicit arguments, else the urls node of the last request. */
    char urls[8192];
    if (arg != NULL && *arg != '\0')
      {
        size_t n = 0;
        for (const char *p = arg; *p != '\0' && n + 2 < sizeof urls; p++)
            urls[n++] = (*p == ' ') ? '\n' : *p;
        urls[n] = '\0';
      }
    else
      {
        narrate ("read", INF_NODE_URLS);
        if (read_node (INF_NODE_URLS, urls, sizeof urls) < 0)
            urls[0] = '\0';
      }
    if (urls[0] == '\0')
      {
        printf ("  usage: /learn [URL ...] — ou soumettez d'abord un prompt "
                "contenant des URL (/submit)\n");
        return;
      }

    for (char *line = strtok (urls, "\n"); line != NULL;
         line = strtok (NULL, "\n"))
      {
        while (*line == ' ' || *line == '\t')
            line++;
        char *e = line + strlen (line);
        while (e > line && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
            *--e = '\0';
        if (*line == '\0')
            continue;
        inf_url_trim (line);
        if (!inf_url_valid (line))
          {
            printf ("  URL refusée: %s\n", line);
            continue;
          }
        long status;
        char *body;
        size_t blen;
        if (fetch_url (line, &status, &body, &blen) != 0)
            continue;
        print_http_status (status);
        if (status < 200 || status >= 300)
          {
            free (body);
            continue;
          }
        char text[262144];
        inf_html_to_text (text, sizeof text, body);
        free (body);
        char sum[65];
        inf_sha256_hex (sum, (const unsigned char *) text, strlen (text));
        char rec[300000];
        inf_training_record (rec, sizeof rec, line, status, text, sum);

        db_store_and_report (rec);
        training_cache_append (rec);
      }
}

/* Write one training record to the db mount root and print the
 * (highlighted) answer: inserted green, duplicate yellow, error red. */
void
db_store_and_report (const char *rec)
{
    if (inf_client_color ())
        printf ("\033[2m\u2192 write %s\033[0m\n", g_db);
    else
        printf ("\u2192 write %s\n", g_db);
    if (write_root_line (g_db, rec, strlen (rec)) != 0)
      {
        printf ("  /db injoignable (%s) — montez db-translator :\n"
                "    settrans -a /db db-translator --conninfo \"dbname=gnuai\"\n",
                strerror (errno));
        return;
      }
    char resp[2048];
    if (inf_client_color ())
        printf ("\033[2m\u2192 read %s\033[0m\n", g_db);
    else
        printf ("\u2192 read %s\n", g_db);
    if (read_root (g_db, resp, sizeof resp) < 0)
        printf ("  pas de réponse de %s\n", g_db);
    else
      {
        resp[strcspn (resp, "\r\n")] = '\0';
        const char *a = "";
        if (inf_client_color ())
            a = strstr (resp, "duplicate") != NULL ? "\033[33m"
              : strstr (resp, "error") != NULL ? "\033[1;31m"
              : "\033[32m";
        char hl[8192];
        inf_json_highlight (hl, sizeof hl, resp, inf_client_color ());
        if (*a)
            printf ("  db: %s%s\033[0m\n", a, hl);
        else
            printf ("  db: %s\n", hl);
      }
}

/* /stats - real-time CPU/memory/IO display: redraw every second,
   any key leaves the monitor. */
void
cmd_stats (void)
{
    char buf[2048];
    bool color = inf_client_color ();
    if (color)
        printf ("\033[2J\033[H");           /* clear + home */
    for (;;)
      {
        inf_client_stats (buf, sizeof buf);
        if (color)
            printf ("\033[H");              /* home, redraw in place */
        printf ("inference - /stats (une touche pour quitter)\n%s", buf);
        fflush (stdout);
        fd_set fds;
        FD_ZERO (&fds);
        FD_SET (STDIN_FILENO, &fds);
        struct timeval tv = { 1, 0 };
        if (select (STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0)
          {
            char c;
            if (read (STDIN_FILENO, &c, 1) <= 0)
                break;
            break;                          /* any key leaves */
          }
      }
    if (color)
        printf ("\033[2J\033[H");
    printf ("\n");
}

void
cmd_help (void)
{
    printf ("  Commandes (une ligne commençant par '/') :\n");
    for (size_t i = 0; i < inf_actions_count; i++)
        printf ("    %-8s %s\n", inf_actions[i].command,
                inf_actions[i].help);
    printf ("  Tâches de /task :");
    for (int t = 0; t <= INF_TASK_CUSTOM; t++)
        printf (" %s", inf_task_name ((enum inf_task) t));
    printf (" auto\n");
    printf ("  Édition de ligne : DEL efface le dernier caractère, "
            "Tab complète\n    (commande ou tâche), '\\' en tête de ligne "
            "échappe le '/' (texte libre).\n");
    printf ("  Nœuds POSIX du translator monté :");
    const char *const *nodes = inf_fs_nodes ();
    for (size_t i = 0; i < inf_fs_node_count (); i++)
        printf (" %s", nodes[i]);
    printf ("\n");
    printf ("  Le prompt soumis est écrit dans <montage>/prompt ; la "
            "requête JSON\n    se lit dans <montage>/request.\n");
    printf ("  Coloration syntaxique (URLs, JSON) sur terminal ; NO_COLOR "
            "la désactive,\n    aucune séquence ANSI ne fuit vers un "
            "non-TTY.\n");
}

void
cmd_task (const char *arg)
{
    enum inf_task t;
    if (arg == NULL || *arg == '\0')
      {
        printf ("  usage: /task TÂCHE (summarize|translate|answer|classify|"
                "custom|auto)\n");
        return;
      }
    if (strcmp (arg, "auto") == 0)
      {
        narrate ("write", INF_NODE_TASK);
        if (write_node (INF_NODE_TASK, arg, strlen (arg)) != 0)
            printf ("  écriture refusée: %s\n", strerror (errno));
        else
            printf ("  détection automatique rétablie (/task auto)\n");
        return;
      }
    if (!inf_task_from_name (arg, strlen (arg), &t))
      {
        printf ("  tâche inconnue: %s (summarize|translate|answer|classify|"
                "custom|auto)\n", arg);
        return;
      }
    narrate ("write", INF_NODE_TASK);
    if (write_node (INF_NODE_TASK, arg, strlen (arg)) != 0)
        printf ("  écriture refusée: %s\n", strerror (errno));
    else
        printf ("  tâche forcée: %s\n", inf_task_name (t));
}

void
cmd_submit (void)
{
    if (g_blen == 0)
      {
        printf ("  rien à soumettre (le buffer est vide)\n");
        return;
      }
    narrate ("write", INF_NODE_PROMPT);
    if (write_node (INF_NODE_PROMPT, g_buf, g_blen) != 0)
      {
        printf ("  écriture refusée: %s\n", strerror (errno));
        return;
      }

    /* Explain the request (pure core on the submitted buffer), then
     * show what the translator actually rendered. */
    struct inf_request req;
    req.prompt = g_buf;
    size_t nurls = inf_parse_prompt (g_buf, &req);
    printf ("  requête soumise: tâche=%s, %zu URL(s)\n",
            inf_task_name (req.task), nurls);

    /* Phase 3: spinner while the translator processes the prompt
     * (poll /inference/status until it leaves its previous state,
     * at most ~10 s), then an atomic highlighted redraw. */
    char before[256] = "";
    (void) read_node (INF_NODE_STATUS, before, sizeof before);
    before[strcspn (before, "\r\n")] = '\0';
    static const char frames[] = "|/-\\";
    printf ("  traitement ");
    fflush (stdout);
    for (int i = 0; i < 100; i++)
      {
        char val[256];
        if (read_node (INF_NODE_STATUS, val, sizeof val) >= 0)
          {
            val[strcspn (val, "\r\n")] = '\0';
            if (strcmp (val, before) != 0)
                break;
          }
        printf ("\r  traitement %c ", frames[i % 4]);
        fflush (stdout);
        usleep (100 * 1000);
      }
    printf ("\r                    \r");
    fflush (stdout);

    char reqjson[INF_JSON_MAX];
    narrate ("read", INF_NODE_REQUEST);
    if (read_node (INF_NODE_REQUEST, reqjson, sizeof reqjson) < 0)
        printf ("  request illisible avant soumission\n");
    else
      {
        char hl[INF_JSON_MAX * 4];
        inf_json_highlight (hl, sizeof hl, reqjson, inf_client_color ());
        printf ("  request: %s\n", hl);
      }

    print_status_node ();

    g_blen = 0;
    g_buf[0] = '\0';
}

void
cmd_editor (void)
{
    char tmpl[] = "/tmp/inf-editor-XXXXXX";
    int fd = mkstemp (tmpl);
    if (fd < 0)
      {
        printf ("  impossible de créer le fichier temporaire: %s\n",
                strerror (errno));
        return;
    }
    if (write (fd, g_buf, g_blen) != (ssize_t) g_blen)
        {
            /* non-fatal: the editor starts with an empty file */
        }
    close (fd);

    const char *editor = inf_client_editor ();
    narrate_path ("write", tmpl);   /* local file, outside the mount */            /* the buffer goes to the editor */
    char cmd[4400];
    snprintf (cmd, sizeof cmd, "%s '%s'", editor, tmpl);
    pid_t pid = fork ();
    if (pid == 0)
      {
        restore_tio ();
        execl ("/bin/sh", "sh", "-c", cmd, (char *) NULL);
        _exit (127);
      }
    int wstatus = 0;
    waitpid (pid, &wstatus, 0);
    if (!WIFEXITED (wstatus) || WEXITSTATUS (wstatus) != 0)
      {
        printf ("  éditeur quitté avec un code non nul: buffer inchangé\n");
        unlink (tmpl);
        return;
      }

    narrate_path ("read", tmpl);    /* local file, outside the mount */             /* re-read the buffer (SPEC.md §5) */
    int rfd = open (tmpl, O_RDONLY);
    if (rfd >= 0)
      {
        ssize_t n = read (rfd, g_buf, INF_PROMPT_MAX - 1);
        close (rfd);
        if (n < 0)
            n = 0;
        g_blen = (size_t) n;
        g_buf[g_blen] = '\0';
      }
    unlink (tmpl);
    printf ("  buffer relu (%zu octets)\n", g_blen);
}

/* --- /read-code: read the code of a URL, detect its language -------- */

#define READ_CODE_PREVIEW_CHARS 8000

void
cmd_read_code (const char *arg)
{
    char urlbuf[2048];
    if (arg == NULL || *arg == '\0')
      {
        printf ("  usage: /read-code URL (http://... ou https://...)\n");
        return;
      }
    snprintf (urlbuf, sizeof urlbuf, "%s", arg);
    inf_url_trim (urlbuf);
    arg = urlbuf;
    if (!inf_url_valid (arg))
      {
        printf ("  URL invalide: http:// ou https:// suivi d'un hôte requis\n");
        return;
      }
    long status;
    char *body;
    size_t blen;
    if (fetch_url (arg, &status, &body, &blen) != 0)
        return;
    print_http_status (status);
    if (status < 200 || status >= 300)
      {
        free (body);
        return;
      }
    const char *lang = inf_lang_detect (arg, body);
    const char *a = inf_client_color () ? "\033[1;36m" : "";
    const char *r = inf_client_color () ? "\033[0m" : "";
    printf ("  langage détecté: %s%s%s (%zu octets, complet dans "
            FETCH_MOUNT "/content)\n", a, lang, r, blen);

    char preview[READ_CODE_PREVIEW_CHARS + 1];
    size_t pl = blen < READ_CODE_PREVIEW_CHARS ? blen : READ_CODE_PREVIEW_CHARS;
    memcpy (preview, body, pl);
    preview[pl] = '\0';
    free (body);
    char hl[READ_CODE_PREVIEW_CHARS * 4];
    inf_code_highlight (hl, sizeof hl, preview, lang, inf_client_color ());
    fputs (hl, stdout);
    if (pl > 0 && preview[pl - 1] != '\n')
        printf ("\n");
}

/* --- /learn-code: /read-code + persist the code into /db ------------- */

void
cmd_learn_code (const char *arg)
{
    char urlbuf[2048];
    if (arg == NULL || *arg == '\0')
      {
        printf ("  usage: /learn-code URL (http://... ou https://...)\n");
        return;
      }
    snprintf (urlbuf, sizeof urlbuf, "%s", arg);
    inf_url_trim (urlbuf);
    if (!inf_url_valid (urlbuf))
      {
        printf ("  URL invalide: http:// ou https:// suivi d'un hôte requis\n");
        return;
      }
    long status;
    char *body;
    size_t blen;
    if (fetch_url (urlbuf, &status, &body, &blen) != 0)
        return;
    print_http_status (status);
    if (status < 200 || status >= 300)
      {
        free (body);
        return;
      }
    const char *lang = inf_lang_detect (urlbuf, body);
    const char *a = inf_client_color () ? "\033[1;36m" : "";
    const char *r = inf_client_color () ? "\033[0m" : "";
    printf ("  langage détecté: %s%s%s (%zu octets)\n", a, lang, r, blen);

    char preview[READ_CODE_PREVIEW_CHARS + 1];
    size_t pl = blen < READ_CODE_PREVIEW_CHARS ? blen : READ_CODE_PREVIEW_CHARS;
    memcpy (preview, body, pl);
    preview[pl] = '\0';
    char hl[READ_CODE_PREVIEW_CHARS * 4];
    inf_code_highlight (hl, sizeof hl, preview, lang, inf_client_color ());
    fputs (hl, stdout);
    if (pl > 0 && preview[pl - 1] != '\n')
        printf ("\n");

    /* persist the raw code as a training element (training_data) */
    char sum[65];
    inf_sha256_hex (sum, (const unsigned char *) body, blen);
    char rec[300000];
    inf_training_record (rec, sizeof rec, urlbuf, status, body, sum);
    free (body);
    db_store_and_report (rec);
    training_cache_append (rec);
}

/* Append one training record line to the local NDJSON cache that
 * /reply searches (best effort). */
int
training_cache_append (const char *rec)
{
    if (g_training_cache == NULL || g_training_cache[0] == '\0')
        return -1;
    int fd = open (g_training_cache, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0)
        return -1;
    size_t l = strlen (rec);
    bool ok = write (fd, rec, l) == (ssize_t) l && write (fd, "\n", 1) == 1;
    close (fd);
    return ok ? 0 : -1;
}

/* --- /reply: answer from the training data ---------------------------- */

/* collect_neurons is defined with /entrainement below; forward-declared
   here so /reply can reuse the neuron probe (/llm1..N). */
#define REPLY_VEC_MAX 64
static int collect_neurons (char llms[][32], int *nllm, unsigned *in_size);
/* defined with the /db commands below; used by /reply so the training
   records come from /db first, the local cache as fallback. */
static ssize_t db_source (char *buf, size_t bufsz);

/* try_extract: when TEXT looks like JSON (object, array, or a fragment
   with "key": boundaries), replace it with the extracted string
   literals.  Returns 1 when TEXT was replaced. */
static int
try_extract (char *text, size_t textsz)
{
    if (text[0] != '{' && text[0] != '[' && strstr (text, "\":") == NULL)
        return 0;
    char ext[65536];
    if (inf_json_strings_extract (text, ext, sizeof ext) == 0
        && ext[0] != '\0')
      {
        snprintf (text, textsz, "%s", ext);
        return 1;
      }
    return 0;
}

/* decode_training_text: presentation decoder for a stored training
   chunk, shared by /reply and /db search.  Pulls the nested text field
   out of a JSON record; when what remains is JSON (a raw .json source
   chunk, possibly still escaped one level), extracts its string
   literals instead of dumping the JSON structure; only then decodes
   leftover escapes in plain text. */
static void
decode_training_text (char *text, size_t textsz)
{
    if (text == NULL || textsz == 0)
        return;
    if (text[0] == '{' || text[0] == '[' || text[0] == '"')
      {
        static const char *fields[] = { "value", "output", "text",
                                        "content", "answer", NULL };
        for (int i = 0; fields[i] != NULL; i++)
          {
            char sub[65536];
            if (inf_json_str_field (text, fields[i], sub, sizeof sub) == 0
                && sub[0] != '\0')
              {
                snprintf (text, textsz, "%s", sub);
                break;
              }
          }
      }
    if (try_extract (text, textsz))
        return;
    if (strstr (text, "\\\"") != NULL || strstr (text, "\\\\") != NULL)
      {
        /* still-escaped chunk (JSON-in-JSON): decode one level, retry */
        inf_json_unescape (text);
        if (try_extract (text, textsz))
            return;
      }
    if (strstr (text, "\\n") != NULL || strstr (text, "\\t") != NULL
        || strstr (text, "\\u") != NULL)
      inf_json_unescape (text);
    /* one JSON-escape level still present (double-stored chunk): drop it */
    if (strstr (text, "\\\\") != NULL || strstr (text, "\\\"") != NULL)
      inf_json_unescape (text);
}

void
cmd_reply (const char *arg)
{
    const char *query = (arg != NULL && *arg != '\0') ? arg : g_buf;
    if (query == NULL || *query == '\0')
      {
        printf ("  usage: /reply [question] — sinon le prompt courant sert de question\n");
        return;
      }
    static char cache[4 * 1024 * 1024];
    ssize_t n = db_source (cache, sizeof cache);
    if (n <= 0)
      {
        printf ("  aucune donnée d'entraînement (/db illisible et cache %s vide)\n"
                "  apprenez d'abord : /learn URL, /entrainement URL, /db sync\n",
                g_training_cache != NULL ? g_training_cache : "?");
        return;
      }

    char best[300000];
    if (inf_training_search (cache, query, best, sizeof best) == 0)
      {
        printf ("  aucune donnée d'entraînement ne correspond à « %s »\n",
                query);
        return;
      }
    char url[2048] = "";
    (void) inf_json_str_field (best, "source_url", url, sizeof url);
    /* /entrainement records carry {"input":..,"output":[..]} as content:
       the neuron output is the reply; plain /learn records reply with
       the stored text. */
    char output[8192] = "";
    char content[262144] = "";
    const char *reply = NULL;
    if (inf_json_str_field (best, "output", output, sizeof output) == 0
        && output[0] != '\0')
        reply = output;
    else if (inf_json_str_field (best, "content", content, sizeof content) == 0)
        reply = content;

    /* Neuron-backed reply: when /llm1..N are mounted, the question plus
       the best matching training excerpt are vectorized and fed to one
       neuron (round-robin); its "Output:" section is the LLM answer and
       the training record is cited as the anchor.  Otherwise fall back
       to the stored text below. */
    {
        char llms[16][32];
        int nllm = 0;
        unsigned in_size = 0;
        if (collect_neurons (llms, &nllm, &in_size) == 0)
          {
            const char *ctx = content[0] != '\0' ? content : output;
            char prompt[8192];
            inf_reply_prompt (prompt, sizeof prompt, query, ctx);
            double vec[REPLY_VEC_MAX];
            size_t vn = inf_text_to_vector (prompt, vec, in_size);
            char incmd[2048];
            inf_vector_command (incmd, sizeof incmd, vec, vn);
            static int turn;
            const char *llm = llms[(turn++) % nllm];
            narrate_path ("write", llm);
            if (write_root (llm, incmd, strlen (incmd)) == 0)
              {
                char out_text[8192];
                narrate_path ("read", llm);
                if (read_root (llm, out_text, sizeof out_text) >= 0)
                  {
                    double vals[REPLY_VEC_MAX];
                    size_t nv = 0;
                    inf_parse_neuron_output (out_text, vals, REPLY_VEC_MAX,
                                             &nv);
                    if (nv > 0)
                      {
                        printf ("  réponse du neurone %s, ancrée sur %s :\n",
                                llm, url[0] != '\0' ? url : "cache local");
                        printf ("  question : %s\n", query);
                        char block[8192];
                        inf_sigmoid_render (block, sizeof block, llm, vals,
                                            nv, inf_client_color ());
                        fputs (block, stdout);
                        char decoded[256];
                        if (inf_vector_to_text (decoded, sizeof decoded,
                                                vals, nv) > 0)
                            printf ("  réponse du LLM (texte décodé) : %s\n",
                                    decoded);
                        return;
                      }
                  }
              }
            printf ("  neurone %s indisponible — repli sur le cache\n", llm);
          }
    }
    printf ("  réponse d'après les données d'entraînement (%s) :\n",
            url[0] != '\0' ? url : "cache local");
    if (reply != NULL)
      {
        /* Presentation: decode_training_text handles JSON-in-JSON content
           (text field of a record, or string literals of a raw JSON
           fragment); then print indented, one line at a time. */
        char text[262144];
        snprintf (text, sizeof text, "%s", reply);
        decode_training_text (text, sizeof text);
        size_t rl = strlen (text);
        if (rl > 2000)
            rl = 2000;
        if (text[0] == '{' || text[0] == '[')
          {
            char hl[8192];
            inf_json_highlight (hl, sizeof hl, text, inf_client_color ());
            printf ("  %s%s\n", hl, rl < strlen (text) ? "…" : "");
          }
        else
          {
            const char *p = text;
            printf ("  ");
            while (*p != '\0' && (size_t) (p - text) < rl)
              {
                if (*p == '\n')
                  {
                    putchar ('\n');
                    printf ("  ");
                  }
                else
                  putchar (*p);
                p++;
              }
            printf ("%s\n", rl < strlen (text) ? "…" : "");
          }
      }
}

/* --- /code: produce a code snippet from the training data -------------- */

/* block_trivial: a fenced block that is only a placeholder ("# Your
   code here", "...") is not an answer. */
static int
block_trivial (const char *block)
{
    size_t n = 0;
    for (const char *p = block; *p != '\0'; p++)
        if (*p != ' ' && *p != '\n' && *p != '\t' && *p != '\r')
            n++;
    return n < 10;
}

/* try_code_hit: does the NDJSON record LINE yield a usable fenced code
   block for LANG (NULL = any language)?  Returns 1 and fills BLOCK and
   URL on success.  Raw chunk first (keeps \n in code intact), then the
   decoded text. */
static int
try_code_hit (const char *line, const char *lang, char *block,
              size_t blocksz, char *url, size_t urlsz)
{
    if (line == NULL || block == NULL || blocksz == 0)
        return 0;
    if (url != NULL && urlsz > 0)
      {
        url[0] = '\0';
        (void) inf_json_str_field (line, "source_url", url, urlsz);
      }
    char output[8192] = "";
    char content[262144] = "";
    const char *reply = NULL;
    if (inf_json_str_field (line, "output", output, sizeof output) == 0
        && output[0] != '\0')
        reply = output;
    else if (inf_json_str_field (line, "content", content, sizeof content) == 0)
        reply = content;
    if (reply == NULL)
        return 0;
    if (inf_extract_code_block (reply, lang, block, blocksz) == 0
        && !block_trivial (block))
        return 1;
    char text[262144];
    snprintf (text, sizeof text, "%s", reply);
    decode_training_text (text, sizeof text);
    if (inf_extract_code_block (text, lang, block, blocksz) == 0
        && !block_trivial (block))
        return 1;
    return 0;
}

void
cmd_code (const char *arg)
{
    if (arg == NULL || *arg == '\0')
      {
        printf ("  usage: /code [langage] description — ex. /code c hello world\n");
        return;
      }
    /* optional leading language token: /code c reverse a string */
    static const char *langs[] = { "c", "c++", "cpp", "python", "py",
        "shell", "sh", "bash", "rust", "go", "javascript", "js", "java",
        "sql", "html", NULL };
    char first[32] = "";
    const char *rest = arg;
    size_t fl = 0;
    while (arg[fl] != '\0' && arg[fl] != ' ' && fl + 1 < sizeof first)
      {
        first[fl] = arg[fl];
        fl++;
      }
    first[fl] = '\0';
    const char *lang = NULL;
    if (first[0] != '\0' && arg[fl] == ' ')
      {
        for (int i = 0; langs[i] != NULL; i++)
          if (strcasecmp (first, langs[i]) == 0)
            {
              lang = langs[i];
              rest = arg + fl;
              while (*rest == ' ')
                rest++;
              break;
            }
      }
    char query[2048];
    snprintf (query, sizeof query, "%s%s%s", lang != NULL ? lang : "",
              lang != NULL ? " " : "", rest);
    static char cache[4 * 1024 * 1024];
    if (db_source (cache, sizeof cache) <= 0)
      {
        printf ("  aucune donnée d'entraînement (/db illisible et cache %s vide)\n"
                "  apprenez d'abord : /learn URL, /entrainement URL, /db sync\n",
                g_training_cache != NULL ? g_training_cache : "?");
        return;
      }
    static char hits[512 * 1024];
    size_t nhits = inf_training_search_list (cache, query, hits, sizeof hits,
                                             20);
    if (nhits == 0 && lang != NULL)
        /* retry without the language prefix: /code c hello world ->
           "hello world" */
        nhits = inf_training_search_list (cache, rest, hits, sizeof hits, 20);
    if (nhits == 0)
      {
        printf ("  aucune donnée d'entraînement ne correspond à « %s »\n",
                query);
        return;
      }
    printf ("  code d'après les données d'entraînement pour « %s » :\n",
            query);
    /* pass 1: fences matching LANG; pass 2: any fence */
    static char block[262144];
    for (int pass = 0; pass < 2; pass++)
      {
        const char *want = (pass == 0 || lang == NULL) ? lang : NULL;
        char *save = NULL;
        for (char *line = strtok_r (hits, "\n", &save); line != NULL;
             line = strtok_r (NULL, "\n", &save))
          {
            char url[2048];
            if (try_code_hit (line, want, block, sizeof block, url,
                              sizeof url))
              {
                printf ("  source : %s\n",
                        url[0] != '\0' ? url : "cache local");
                const char *hl_lang = want != NULL ? want
                                  : inf_lang_detect (url, block);
                static char hl[262144 * 4];
                inf_code_highlight (hl, sizeof hl, block, hl_lang,
                                    inf_client_color ());
                fputs (hl, stdout);
                size_t bl = strlen (block);
                if (bl > 0 && block[bl - 1] != '\n')
                    printf ("\n");
                return;
              }
          }
        /* strtok_r consumed hits: restore for pass 2 */
        if (pass == 0)
          {
            /* rebuild hits from the cache (strtok_r zeroed the separators) */
            nhits = inf_training_search_list (cache, query, hits,
                                             sizeof hits, 20);
            if (nhits == 0 && lang != NULL)
                nhits = inf_training_search_list (cache, rest, hits,
                                                  sizeof hits, 20);
          }
      }
    /* no usable fenced block: show the decoded best hit, capped
       (rebuild hits: strtok_r zeroed the separators) */
    inf_training_search_list (cache, query, hits, sizeof hits, 20);
    if (lang != NULL)
        inf_training_search_list (cache, rest, hits, sizeof hits, 20);
    char firstline[300000];
    {
        char *save = NULL;
        char *line = strtok_r (hits, "\n", &save);
        snprintf (firstline, sizeof firstline, "%s",
                  line != NULL ? line : "");
    }
    char url[2048] = "";
    char content[262144] = "";
    const char *reply = firstline;
    char output[8192] = "";
    if (inf_json_str_field (firstline, "output", output, sizeof output) == 0
        && output[0] != '\0')
        reply = output;
    else if (inf_json_str_field (firstline, "content", content,
                                 sizeof content) == 0)
        reply = content;
    char text[262144];
    snprintf (text, sizeof text, "%s", reply);
    decode_training_text (text, sizeof text);
    (void) inf_json_str_field (firstline, "source_url", url, sizeof url);
    printf ("  source : %s (aucun bloc de code, texte décodé) :\n",
            url[0] != '\0' ? url : "cache local");
    size_t rl = strlen (text);
    if (rl > 2000)
        rl = 2000;
    const char *p = text;
    printf ("  ");
    while (*p != '\0' && (size_t) (p - text) < rl)
      {
        if (*p == '\n')
            printf ("\n  ");
        else
            putchar (*p);
        p++;
      }
    printf ("%s\n", rl < strlen (text) ? "…" : "");
}

/* --- /sigmoid: visualize the state of the mounted neurons ------------- */

void
cmd_sigmoid (void)
{
    printf ("  état des neurones (sigmoïde des sorties courantes) :\n");
    int found = 0;
    for (int i = 1; i <= 16; i++)
      {
        char mount[32];
        snprintf (mount, sizeof mount, "/llm%d", i);
        char text[8192];
        narrate_path ("read", mount);
        if (read_root (mount, text, sizeof text) < 0)
            continue;
        double vals[64];
        size_t n = 0;
        inf_parse_neuron_output (text, vals, 64, &n);
        if (n == 0)
            continue;
        found++;
        char block[8192];
        inf_sigmoid_render (block, sizeof block, mount, vals, n,
                            inf_client_color ());
        fputs (block, stdout);
      }
    if (found == 0)
        printf ("  aucun neurone monté (/llm1..N illisibles)\n"
                "  montez-en un : settrans -a /llm1 neuron-translator ...\n");
}

/* --- /entrainement: fill /db through the neuron-translator ------------
   Accepts a page URL (http/https) OR an hf:// URL (bucket/dataset/model):
   the hf:// files are downloaded into a local dir (like /hf sync), then
   every chunk is fed to a neuron before being stored in /db. */

#define TRAIN_MAX_CHUNKS 64
#define TRAIN_CHUNK_MAX  1024
#define TRAIN_VEC_MAX    64

/* neuron_input_size — parse "input v1,...,v%u" from a neuron status
   text; returns 0 when unknown. */
static unsigned
neuron_input_size (const char *text)
{
    const char *p = strstr (text, "input v1,...,v");
    if (p == NULL)
        return 0;
    p += strlen ("input v1,...,v");
    return (unsigned) strtoul (p, NULL, 10);
}

/* collect_neurons — probe /llm1../llm16 into LLMS; *in_size gets the input
   layer size parsed from the last status text.  0 on success, -1 when no
   neuron answers or the input size is unknown/unsupported. */
static int
collect_neurons (char llms[][32], int *nllm, unsigned *in_size)
{
    char probe[8192];
    *nllm = 0;
    *in_size = 0;
    for (int i = 1; i <= 16 && *nllm < 16; i++)
      {
        snprintf (llms[*nllm], sizeof llms[0], "/llm%d", i);
        if (read_root (llms[*nllm], probe, sizeof probe) >= 0)
            (*nllm)++;
      }
    if (*nllm == 0)
        return -1;
    *in_size = neuron_input_size (probe);
    if (*in_size == 0 || *in_size > TRAIN_VEC_MAX)
        return -1;
    return 0;
}

static void hf_train (const char *hfurl, const char *localdir);
static char *hf_fetch_list (const char *hfurl, long *out_status);
static bool path_safe (const char *path);

/* train_text — split TEXT into chunks, feed each chunk through a neuron
   (llms[c % nllm]), and store {"input":..,"output":[..],"neuron":..} in
   /db (training_data) + the local cache.  Returns the records stored. */
static size_t
train_text (const char *source_url, long http_status, const char *text,
            char llms[][32], int nllm, unsigned in_size)
{
    static const char *chunks[TRAIN_MAX_CHUNKS];
    static size_t clens[TRAIN_MAX_CHUNKS];
    size_t nchunks = 0;
    {
        const char *p = text;
        while (*p != '\0' && nchunks < TRAIN_MAX_CHUNKS)
          {
            while (*p == '\n' || *p == ' ' || *p == '\t' || *p == '\r')
                p++;
            if (*p == '\0')
                break;
            const char *start = p;
            const char *end = p;
            while (*end != '\0')
              {
                if (end[0] == '\n' && (end[1] == '\n' || end[1] == '\0'))
                    break;
                if ((size_t) (end - start) >= TRAIN_CHUNK_MAX)
                  {
                    const char *sp = end;
                    while (sp > start && *sp != ' ' && *sp != '\n')
                        sp--;
                    end = sp > start ? sp : end;
                    break;
                  }
                end++;
              }
            size_t cl = (size_t) (end - start);
            while (cl > 0 && (start[cl - 1] == '\n' || start[cl - 1] == ' '
                              || start[cl - 1] == '\r'))
                cl--;
            if (cl > 0)
              {
                chunks[nchunks] = start;
                clens[nchunks] = cl;
                nchunks++;
              }
            p = end;
          }
    }
    if (nchunks == 0)
        return 0;
    printf ("  %zu extrait(s) à faire passer par %d neurone(s) "
            "(entrée %u)\n", nchunks, nllm, in_size);

    size_t nrec = 0;
    for (size_t c = 0; c < nchunks; c++)
      {
        char chunk[TRAIN_CHUNK_MAX + 1];
        memcpy (chunk, chunks[c], clens[c]);
        chunk[clens[c]] = '\0';

        const char *llm = llms[c % nllm];
        double vec[TRAIN_VEC_MAX];
        inf_text_to_vector (chunk, vec, in_size);

        char incmd[2048];
        size_t off = 0;
        off += (size_t) snprintf (incmd + off, sizeof incmd - off, "input ");
        for (unsigned i = 0; i < in_size; i++)
            off += (size_t) snprintf (incmd + off, sizeof incmd - off,
                                      "%s%.6f", i > 0 ? "," : "", vec[i]);
        narrate_path ("write", llm);
        if (write_root (llm, incmd, strlen (incmd)) != 0)
          {
            printf ("  extrait %zu: écriture sur %s refusée (%s) — "
                    "chmod 666 %s ou root\n", c + 1, llm, strerror (errno),
                    llm);
            continue;
          }
        char out_text[8192];
        narrate_path ("read", llm);
        if (read_root (llm, out_text, sizeof out_text) < 0)
          {
            printf ("  extrait %zu: sortie de %s illisible\n", c + 1, llm);
            continue;
          }
        double vals[TRAIN_VEC_MAX];
        size_t nv = 0;
        inf_parse_neuron_output (out_text, vals, TRAIN_VEC_MAX, &nv);
        if (nv == 0)
          {
            printf ("  extrait %zu: aucune sortie de %s\n", c + 1, llm);
            continue;
          }

        /* content = {"input":..,"output":[..],"neuron":"/llmK"} */
        char ein[TRAIN_CHUNK_MAX * 6 + 8];
        inf_json_escape (chunk, ein, sizeof ein);
        char outj[2048];
        size_t oj = 0;
        oj += (size_t) snprintf (outj + oj, sizeof outj - oj, "[");
        for (size_t i = 0; i < nv; i++)
            oj += (size_t) snprintf (outj + oj, sizeof outj - oj, "%s%.6f",
                                     i > 0 ? "," : "", vals[i]);
        snprintf (outj + oj, sizeof outj - oj, "]");
        char content[6 * TRAIN_CHUNK_MAX + 2200];
        snprintf (content, sizeof content,
                  "{\"input\":\"%s\",\"output\":%s,\"neuron\":\"%s\"}",
                  ein, outj, llm);
        char sum[65];
        inf_sha256_hex (sum, (const unsigned char *) content,
                        strlen (content));
        char rec[300000];
        inf_training_record (rec, sizeof rec, source_url, http_status,
                             content, sum);
        printf ("  extrait %zu/%zu -> %s, sortie [%zu valeurs]\n",
                c + 1, nchunks, llm, nv);
        db_store_and_report (rec);
        training_cache_append (rec);
        nrec++;
      }
    return nrec;
}

static void
train_neurons_or_die (char llms[][32], int *nllm, unsigned *in_size)
{
    if (collect_neurons (llms, nllm, in_size) != 0)
      {
        printf ("  aucun neurone monté (/llm1..N) ou entrée illisible : "
                "/entrainement fait passer chaque extrait par le "
                "neuron-translator\n"
                "  settrans -a /llm1 neuron-translator ...\n");
        *nllm = 0;
      }
}

void
cmd_training (const char *arg)
{
    char urlbuf[2048], dirbuf[1024] = "";
    if (arg == NULL || *arg == '\0')
      {
        printf ("  usage: /entrainement URL|hf://... [dossier-local]\n"
                "    URL      : page de dataset (ex. https://huggingface.co/datasets)\n"
                "    hf://…   : bucket/dataset/model HF (ex. hf://buckets/<owner>/<bucket>),\n"
                "               téléchargé dans [dossier-local] (défaut ./data)\n");
        return;
      }
    int nf = sscanf (arg, "%2047s %1023s", urlbuf, dirbuf);
    if (nf < 1)
      {
        printf ("  usage: /entrainement URL|hf://... [dossier-local]\n");
        return;
      }
    inf_url_trim (urlbuf);
    if (inf_hf_url (urlbuf))
      {
        hf_train (urlbuf, nf >= 2 && dirbuf[0] != '\0' ? dirbuf : "./data");
        return;
      }
    if (!inf_url_valid (urlbuf))
      {
        printf ("  URL invalide: http://, https:// ou hf:// requis\n");
        return;
      }
    long status;
    char *body;
    size_t blen;
    if (fetch_url (urlbuf, &status, &body, &blen) != 0)
        return;
    print_http_status (status);
    if (status < 200 || status >= 300)
      {
        free (body);
        return;
      }
    char text[262144];
    inf_html_to_text (text, sizeof text, body);
    free (body);
    if (text[0] == '\0')
      {
        printf ("  page sans texte exploitable\n");
        return;
      }
    static char llms[16][32];
    int nllm = 0;
    unsigned in_size = 0;
    train_neurons_or_die (llms, &nllm, &in_size);
    if (nllm == 0)
        return;
    size_t n = train_text (urlbuf, status, text, llms, nllm, in_size);
    printf ("  %zu enregistrement(s) d'entraînement écrits dans /db\n", n);
}

/* --- /hf: Hugging Face hf:// over plain HTTP (httpfs + /etc/hosts auto) - */

/* path_safe — reject path traversal in bucket file paths. */
static bool
path_safe (const char *path)
{
    if (path == NULL || *path == '\0' || *path == '/')
        return false;
    const char *p = path;
    while (*p != '\0')
      {
        const char *seg = p;
        while (*p != '\0' && *p != '/')
            p++;
        size_t sl = (size_t) (p - seg);
        if (sl == 2 && seg[0] == '.' && seg[1] == '.')
            return false;
        if (*p == '/')
            p++;
      }
    return true;
}

/* hf_fetch_list — fetch the HF tree API for HFURL, following the
   Link rel="next" pagination (the API returns at most 1000 entries per
   page); returns a malloc'd JSON array body (caller frees) or NULL. */
static char *
hf_fetch_list (const char *hfurl, long *out_status)
{
    char api[2048];
    if (inf_hf_api_url (hfurl, api, sizeof api) == 0)
      {
        printf ("  URL hf:// invalide (attendu: hf://buckets|datasets|models/<owner>/<repo>[/prefix])\n");
        return NULL;
      }
    char *acc = malloc (2);
    if (acc == NULL)
        return NULL;
    acc[0] = '[';
    acc[1] = '\0';
    size_t alen = 1, acap = 2;
    char url[2200];
    snprintf (url, sizeof url, "%s", api);
    long status = 0;
    for (int page = 0; page < 64; page++)   /* hard cap: 64 pages */
      {
        char *body;
        size_t blen;
        char headers[16384];
        if (fetch_url_h (url, &status, &body, &blen, headers, sizeof headers) != 0)
          {
            free (acc);
            return NULL;
          }
        print_http_status (status);
        if (status < 200 || status >= 300)
          {
            free (body);
            free (acc);
            return NULL;
          }
        if (inf_hf_array_merge (&acc, &alen, &acap, body) == 0)
          {
            free (body);
            free (acc);
            return NULL;
          }
        free (body);
        char next[2048];
        if (inf_hf_link_next (headers, next, sizeof next) == 0)
            break;                        /* last page */
        snprintf (url, sizeof url, "%s", next);
      }
    *out_status = status;
    return acc;
}

static void
hf_list (const char *hfurl)
{
    char kind[16], owner[256], repo[256], prefix[1024];
    if (!inf_hf_parse (hfurl, kind, sizeof kind, owner, sizeof owner,
                       repo, sizeof repo, prefix, sizeof prefix))
        return;
    long status;
    char *tree = hf_fetch_list (hfurl, &status);
    if (tree == NULL)
        return;
    size_t n = inf_json_array_count (tree);
    printf ("  %zu entrée(s) dans %s :\n", n, hfurl);
    for (size_t i = 0; i < n && i < 200; i++)
      {
        char obj[4096];
        if (inf_json_array_object (tree, i, obj, sizeof obj) == 0)
            break;
        char path[1024] = "", type[32] = "";
        double size = 0;
        (void) inf_json_str_field (obj, "path", path, sizeof path);
        (void) inf_json_str_field (obj, "type", type, sizeof type);
        (void) inf_json_num_field (obj, "size", &size);
        if (!inf_hf_path_match (prefix, path))
            continue;
        printf ("    %-6s %8.0f  %s\n", type, size, path);
      }
    free (tree);
}

/* hf_ingest_text — convert one training text into DB-readable records:
   chunked, checksummed, written to /db (training_data) + local cache. */
static size_t
hf_ingest_text (const char *source_url, long http_status, const char *text)
{
    size_t nrec = 0;
    const char *p = text;
    while (*p != '\0' && nrec < 64)
      {
        while (*p == '\n' || *p == ' ' || *p == '\r')
            p++;
        if (*p == '\0')
            break;
        const char *start = p;
        const char *end = p;
        while (*end != '\0' && (size_t) (end - start) < 8000)
          {
            if (end[0] == '\n' && (end[1] == '\n' || end[1] == '\0'))
              {
                end++;
                break;
              }
            end++;
          }
        size_t cl = (size_t) (end - start);
        while (cl > 0 && (start[cl - 1] == '\n' || start[cl - 1] == ' '
                          || start[cl - 1] == '\r'))
            cl--;
        if (cl == 0)
            break;
        char chunk[8001];
        memcpy (chunk, start, cl);
        chunk[cl] = '\0';
        char sum[65];
        inf_sha256_hex (sum, (const unsigned char *) chunk, cl);
        char rec[300000];
        inf_training_record (rec, sizeof rec, source_url, http_status,
                             chunk, sum);
        db_store_and_report (rec);
        training_cache_append (rec);
        nrec++;
        p = end;
      }
    return nrec;
}

static void
hf_sync (const char *hfurl, const char *localdir)
{
    char kind[16], owner[256], repo[256], prefix[1024];
    if (!inf_hf_parse (hfurl, kind, sizeof kind, owner, sizeof owner,
                       repo, sizeof repo, prefix, sizeof prefix))
      {
        printf ("  URL hf:// invalide\n");
        return;
      }
    char q[2100];
    shell_quote (q, sizeof q, localdir);
    char cmd[4400];
    snprintf (cmd, sizeof cmd, "mkdir -p %s", q);
    if (system (cmd) != 0)
      {
        printf ("  impossible de créer %s\n", localdir);
        return;
      }
    long status;
    char *tree = hf_fetch_list (hfurl, &status);
    if (tree == NULL)
        return;
    size_t n = inf_json_array_count (tree);
    size_t nfiles = 0, nrec = 0;
    printf ("  sync de %s vers %s (%zu entrée(s))\n", hfurl, localdir, n);
    for (size_t i = 0; i < n && i < 200; i++)
      {
        char obj[4096];
        if (inf_json_array_object (tree, i, obj, sizeof obj) == 0)
            break;
        char path[1024] = "", type[32] = "";
        (void) inf_json_str_field (obj, "path", path, sizeof path);
        (void) inf_json_str_field (obj, "type", type, sizeof type);
        if (!inf_hf_path_match (prefix, path))
            continue;
        if (strcmp (type, "file") != 0 || !path_safe (path))
            continue;
        char resolve[2048];
        if (inf_hf_resolve_url (hfurl, path, resolve, sizeof resolve) == 0)
            continue;
        char *body;
        size_t blen;
        long fstatus;
        if (fetch_url (resolve, &fstatus, &body, &blen) != 0)
            continue;
        print_http_status (fstatus);
        if (fstatus < 200 || fstatus >= 300)
          {
            free (body);
            continue;
          }
        /* write the local copy */
        char lp[4200];
        snprintf (lp, sizeof lp, "%s/%s", localdir, path);
        char *slash;
        for (slash = lp + strlen (localdir); *slash != '\0'; slash++)
            if (*slash == '/' && slash[1] != '\0')
              {
                *slash = '\0';
                mkdir (lp, 0755);
                *slash = '/';
              }
        int fd = open (lp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0)
          {
            size_t off = 0;
            while (off < blen)
              {
                ssize_t wn = write (fd, body + off, blen - off);
                if (wn <= 0)
                    break;
                off += (size_t) wn;
              }
            close (fd);
          }
        /* convert the training data into DB-readable records */
        char text[262144];
        inf_html_to_text (text, sizeof text, body);
        nrec += hf_ingest_text (resolve, fstatus, text);
        free (body);
        nfiles++;
        printf ("  [%zu/%zu] %s -> %s (%zu octets)\n", nfiles, n, path, lp,
                blen);
      }
    free (tree);
    printf ("  sync terminé: %zu fichier(s), %zu enregistrement(s) dans /db\n",
            nfiles, nrec);
}

void
cmd_hf (const char *arg)
{
    if (arg == NULL || *arg == '\0')
      {
        printf ("  usage:\n"
                "    /hf list hf://buckets/<owner>/<bucket>[/prefix]\n"
                "    /hf sync hf://buckets/<owner>/<bucket>[/prefix] <dossier-local>\n"
                "  (aussi hf://datasets/... et hf://models/... ; sync convertit\n"
                "   les fichiers en enregistrements /db training_data)\n");
        return;
      }
    char sub[16] = "", url[2048] = "", dir[1024] = "";
    int nf = sscanf (arg, "%15s %2047s %1023s", sub, url, dir);
    if (nf < 2)
      {
        printf ("  usage: /hf list <hf://...>  ou  /hf sync <hf://...> <dossier>\n");
        return;
      }
    if (strcmp (sub, "list") == 0)
        hf_list (url);
    else if (strcmp (sub, "sync") == 0)
      {
        if (nf < 3)
          {
            printf ("  usage: /hf sync <hf://...> <dossier-local>\n");
            return;
          }
        hf_sync (url, dir);
      }
    else
        printf ("  sous-commande inconnue: %s (list | sync)\n", sub);
}

/* --- /db: explore and search the database ------------------------------ */

/* db_status — write {"status": true} to /db, read and show the answer. */
static void
db_status (void)
{
    const char *req = "{\"status\": true}";
    if (inf_client_color ())
        printf ("\033[2m→ write %s\033[0m\n", g_db);
    else
        printf ("→ write %s\n", g_db);
    if (write_root_line (g_db, req, strlen (req)) != 0)
      {
        printf ("  /db injoignable (%s) — montez db-translator :\n"
                "    settrans -a /db db-translator --conninfo \"dbname=gnuai\"\n",
                strerror (errno));
        return;
      }
    char resp[8192];
    if (inf_client_color ())
        printf ("\033[2m→ read %s\033[0m\n", g_db);
    else
        printf ("→ read %s\n", g_db);
    if (read_root (g_db, resp, sizeof resp) < 0)
      {
        printf ("  pas de réponse de %s\n", g_db);
        return;
      }
    resp[strcspn (resp, "\r\n")] = '\0';
    char hl[32768];
    inf_json_highlight (hl, sizeof hl, resp, inf_client_color ());
    printf ("  db: %s\n", hl);
}

/* load the training cache (the local mirror of /db training_data). */
static ssize_t
load_training_cache (char *buf, size_t bufsz)
{
    if (g_training_cache == NULL || g_training_cache[0] == '\0')
        return -1;
    int fd = open (g_training_cache, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read (fd, buf, bufsz - 1);
    close (fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return n;
}

/* db_load_records — load training_data records from /db into BUF as
   NDJSON lines (one JSON object per line, each with a "content" field):
   1. the /db/training_data node (data-base netfs, phase 2+);
   2. the {"select": …} line protocol on /db (phase 2);
   3. psql(1) when available (a phase 1 /db serves no reads).
   Returns the length written, or -1 when /db is unreachable or serves
   nothing usable (the caller then falls back to the local cache). */
static ssize_t
db_load_records (char *buf, size_t bufsz)
{
    char path[256];
    snprintf (path, sizeof path, "%s/training_data", g_db);
    ssize_t n = read_root (path, buf, bufsz - 1);
    if (n >= 0)
      {
        buf[n] = '\0';
        if (strchr (buf, '{') != NULL)
            return n;
      }
    const char *req = "{\"select\":{\"table\":\"training_data\",\"limit\":100000}}";
    if (write_root_line (g_db, req, strlen (req)) == 0)
      {
        n = read_root (g_db, buf, bufsz - 1);
        if (n > 0)
          {
            buf[n] = '\0';
            if (strstr (buf, "content") != NULL)
                return n;
          }
      }
    const char *dbn = getenv ("INFERENCE_PSQL_DB");
    if (dbn == NULL || *dbn == '\0')
        dbn = "gnuai";
    char cmd[320];
    snprintf (cmd, sizeof cmd,
              "psql -d %s -tA -c \"select row_to_json(t) from training_data t "
              "order by id\" 2>/dev/null", dbn);
    FILE *pp = popen (cmd, "r");
    if (pp == NULL)
        return -1;
    size_t w = 0;
    char line[300000];
    while (w + 1 < bufsz && fgets (line, sizeof line, pp) != NULL)
      {
        size_t l = strlen (line);
        if (l > 0 && line[l - 1] == '\n')
            l--;
        if (l == 0 || strchr (line, '{') == NULL)
            continue;
        if (w + l + 1 >= bufsz)
            break;
        memcpy (buf + w, line, l);
        w += l;
        buf[w++] = '\n';
      }
    pclose (pp);
    buf[w] = '\0';
    return w > 0 ? (ssize_t) w : -1;
}

/* db_source — the training records behind /reply and /db search|list:
   /db first (source of truth), the local NDJSON cache as fallback. */
static ssize_t
db_source (char *buf, size_t bufsz)
{
    ssize_t n = db_load_records (buf, bufsz);
    if (n > 0)
        return n;
    return load_training_cache (buf, bufsz);
}

static void
db_search (const char *query)
{
    static char cache[4 * 1024 * 1024];
    if (db_source (cache, sizeof cache) <= 0)
      {
        printf ("  cache d'entraînement vide (%s) — /learn, /entrainement "
                "ou /hf sync d'abord\n",
                g_training_cache != NULL ? g_training_cache : "?");
        return;
      }
    static char hits[512 * 1024];
    size_t n = inf_training_search_list (cache, query, hits, sizeof hits,
                                         50);
    printf ("  %zu enregistrement(s) de /db correspondent à « %s » :\n",
            n, query);
    size_t shown = 0;
    for (char *line = strtok (hits, "\n"); line != NULL && shown < 20;
         line = strtok (NULL, "\n"))
      {
        char url[2048] = "", content[262144];
        (void) inf_json_str_field (line, "source_url", url, sizeof url);
        if (inf_json_str_field (line, "content", content, sizeof content)
            != 0)
            content[0] = '\0';
        /* presentation: same decoding as /reply, then collapse newlines
           so the preview stays on one line */
        decode_training_text (content, sizeof content);
        for (char *p = content; *p != '\0'; p++)
          if (*p == '\n' || *p == '\r' || *p == '\t')
            *p = ' ';
        content[240] = '\0';
        printf ("    %s\n      %s%s\n", url[0] != '\0' ? url : "(sans url)",
                content, strlen (content) >= 240 ? "…" : "");
        shown++;
      }
}

static void
db_list (const char *nstr)
{
    static char cache[4 * 1024 * 1024];
    if (db_source (cache, sizeof cache) <= 0)
      {
        printf ("  cache vide (%s)\n",
                g_training_cache != NULL ? g_training_cache : "?");
        return;
      }
    long max = nstr != NULL && *nstr != '\0' ? strtol (nstr, NULL, 10)
                                              : 100;
    if (max <= 0 || max > 1000)
        max = 100;
    /* count lines, then show the last MAX */
    size_t total = 0;
    for (const char *p = cache; *p != '\0'; p++)
        if (*p == '\n')
            total++;
    printf ("  %zu enregistrement(s) dans le cache de /db, les %ld derniers :\n",
            total, max);
    size_t skip = total > (size_t) max ? total - (size_t) max : 0;
    size_t i = 0, shown = 0;
    for (char *line = strtok (cache, "\n"); line != NULL;
         line = strtok (NULL, "\n"))
      {
        if (i++ < skip)
            continue;
        char url[2048] = "", sum[65] = "";
        (void) inf_json_str_field (line, "source_url", url, sizeof url);
        (void) inf_json_str_field (line, "checksum", sum, sizeof sum);
        printf ("    %s  [%s]\n", url[0] != '\0' ? url : "(sans url)",
                sum[0] != '\0' ? sum : "?");
        if (++shown >= (size_t) max)
            break;
      }
}

void
cmd_db (const char *arg)
{
    char sub[16] = "";
    const char *rest = "";
    if (arg != NULL && *arg != '\0')
      {
        int nf = sscanf (arg, "%15s", sub);
        if (nf == 1)
          {
            rest = arg + strlen (sub);
            while (*rest == ' ')
                rest++;
          }
      }
    if (sub[0] == '\0' || strcmp (sub, "status") == 0)
        db_status ();
    else if (strcmp (sub, "search") == 0)
      {
        if (*rest == '\0')
          {
            printf ("  usage: /db search MOT [MOT...]\n");
            return;
          }
        db_search (rest);
      }
    else if (strcmp (sub, "list") == 0)
        db_list (rest);
    else if (strcmp (sub, "sync") == 0)
      {
        /* pull the training_data records from /db into the local cache,
           skipping checksums already present (anti-duplicate) */
        static char recs[4 * 1024 * 1024];
        ssize_t n = db_load_records (recs, sizeof recs);
        if (n <= 0)
          {
            printf ("  /db ne sert aucune lecture ici (phase 1) et psql est indisponible\n"
                    "  compteur direct : psql -d gnuai -tAc 'select count(*) from training_data'\n");
            return;
          }
        static char existing[4 * 1024 * 1024];
        ssize_t en = load_training_cache (existing, sizeof existing);
        if (en < 0)
          {
            en = 0;
            existing[0] = '\0';
          }
        static char fresh[4 * 1024 * 1024];
        size_t fl = inf_training_filter_new (fresh, sizeof fresh, recs,
                                             existing);
        size_t added = 0;
        for (const char *p = fresh; *p != '\0'; p++)
            if (*p == '\n')
                added++;
        if (added > 0 && g_training_cache != NULL && g_training_cache[0] != '\0')
          {
            int fd = open (g_training_cache, O_WRONLY | O_CREAT | O_APPEND,
                           0600);
            if (fd >= 0)
              {
                if (write (fd, fresh, fl) != (ssize_t) fl)
                    printf ("  avertissement : append partiel du cache\n");
                close (fd);
              }
          }
        printf ("  /db sync : %zu nouvel(s) enregistrement(s) ajout\u00e9(s) au cache %s\n",
                added, g_training_cache != NULL ? g_training_cache : "?");
      }
    else if (strcmp (sub, "count") == 0)
      {
        /* real counter from the db translator when mounted */
        const char *req = "{\"status\": true}";
        if (write_root_line (g_db, req, strlen (req)) == 0)
          {
            char resp[8192];
            if (read_root (g_db, resp, sizeof resp) >= 0)
              {
                double n = 0;
                resp[strcspn (resp, "\r\n")] = '\0';
                if (inf_json_num_field (resp, "training_data", &n) == 0)
                    printf ("  /db: %.0f enregistrement(s) dans training_data\n",
                            n);
              }
          }
        static char cache[4 * 1024 * 1024];
        ssize_t n = load_training_cache (cache, sizeof cache);
        size_t total = 0;
        if (n > 0)
            for (const char *p = cache; *p != '\0'; p++)
                if (*p == '\n')
                    total++;
        printf ("  %zu enregistrement(s) d'entraînement (cache local de /db)\n",
                total);
      }
    else
        printf ("  usage: /db status | /db search MOT... | /db list [N] | /db count | /db sync\n");
}

/* hf_train — /entrainement hf://... [dir]: download every file of the
   hf:// repo into localdir (default ./data, like `hf sync ./data hf://…`),
   then feed the content of each file through the neuron translator and
   store {input, output, neuron} records in /db + the local cache. */
static void
hf_train (const char *hfurl, const char *localdir)
{
    char kind[16], owner[256], repo[256], prefix[1024];
    if (!inf_hf_parse (hfurl, kind, sizeof kind, owner, sizeof owner,
                       repo, sizeof repo, prefix, sizeof prefix))
      {
        printf ("  URL hf:// invalide (attendu: hf://buckets|datasets|models/<owner>/<repo>[/prefix])\n");
        return;
      }
    char q[2100];
    char cmd[4400];
    shell_quote (q, sizeof q, localdir);
    snprintf (cmd, sizeof cmd, "mkdir -p %s", q);
    if (system (cmd) != 0)
      {
        printf ("  impossible de créer %s\n", localdir);
        return;
      }

    static char llms[16][32];
    int nllm = 0;
    unsigned in_size = 0;
    train_neurons_or_die (llms, &nllm, &in_size);
    if (nllm == 0)
        return;

    long status;
    char *tree = hf_fetch_list (hfurl, &status);
    if (tree == NULL)
        return;
    size_t n = inf_json_array_count (tree);
    size_t nfiles = 0, nrec = 0;
    printf ("  /entrainement de %s vers %s (%zu entrée(s)), via %d neurone(s)\n",
            hfurl, localdir, n, nllm);
    for (size_t i = 0; i < n && i < 200; i++)
      {
        char obj[4096];
        if (inf_json_array_object (tree, i, obj, sizeof obj) == 0)
            break;
        char path[1024] = "", type[32] = "";
        (void) inf_json_str_field (obj, "path", path, sizeof path);
        (void) inf_json_str_field (obj, "type", type, sizeof type);
        if (!inf_hf_path_match (prefix, path))
            continue;
        if (strcmp (type, "file") != 0 || !path_safe (path))
            continue;
        char resolve[2048];
        if (inf_hf_resolve_url (hfurl, path, resolve, sizeof resolve) == 0)
            continue;
        char *body;
        size_t blen;
        long fstatus;
        if (fetch_url (resolve, &fstatus, &body, &blen) != 0)
            continue;
        print_http_status (fstatus);
        if (fstatus < 200 || fstatus >= 300)
          {
            free (body);
            continue;
          }
        /* local copy under localdir (path traversal rejected by path_safe) */
        char lp[4200];
        snprintf (lp, sizeof lp, "%s/%s", localdir, path);
        char *slash;
        for (slash = lp + strlen (localdir); *slash != '\0'; slash++)
            if (*slash == '/' && slash[1] != '\0')
              {
                *slash = '\0';
                mkdir (lp, 0755);
                *slash = '/';
              }
        int fd = open (lp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0)
          {
            size_t off = 0;
            while (off < blen)
              {
                ssize_t wn = write (fd, body + off, blen - off);
                if (wn <= 0)
                    break;
                off += (size_t) wn;
              }
            close (fd);
          }
        char text[262144];
        inf_html_to_text (text, sizeof text, body);
        free (body);
        if (text[0] != '\0')
            nrec += train_text (resolve, fstatus, text, llms, nllm, in_size);
        nfiles++;
        printf ("  [%zu/%zu] %s -> %s (%zu octets)\n", nfiles, n, path, lp,
                blen);
      }
    free (tree);
    printf ("  /entrainement terminé: %zu fichier(s), %zu enregistrement(s) "
            "dans /db\n", nfiles, nrec);
}
