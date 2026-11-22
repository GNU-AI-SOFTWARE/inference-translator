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
#define VERSION "0.1.0"
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
#define FETCH_MAX_BODY  (256 * 1024)
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

/* fetch_url — mount httpfs on /page for URL, wait for the HEAD probe,
 * read the body.  Returns 0 when an HTTP status was obtained (even
 * "000": the probe failed, *out_status == 0 and *out_body == NULL),
 * -1 when the mount/probe failed.  On success *out_body is malloc'd
 * (NULL when there is no body); the caller frees it. */
static int
fetch_url (const char *url, long *out_status, char **out_body,
           size_t *out_len)
{
    *out_body = NULL;
    *out_len = 0;
    char q[2048];
    char b[2048];
    char cmd[4600];
    shell_quote (q, sizeof q, url);
    shell_quote (b, sizeof b, find_httpfs ());
    snprintf (cmd, sizeof cmd,
              "settrans -fg " FETCH_MOUNT " 2>/dev/null; "
              "sleep 1; "
              "mkdir -p " FETCH_MOUNT "; "
              "settrans -a " FETCH_MOUNT " %s %s", b, q);
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
    return 0;
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
    if (write_root (g_db, rec, strlen (rec)) != 0)
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
    printf ("\n");
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
                "custom)\n");
        return;
      }
    if (!inf_task_from_name (arg, strlen (arg), &t))
      {
        printf ("  tâche inconnue: %s (summarize|translate|answer|classify|"
                "custom)\n", arg);
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
    narrate ("write", tmpl);            /* the buffer goes to the editor */
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

    narrate ("read", tmpl);             /* re-read the buffer (SPEC.md §5) */
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
}
