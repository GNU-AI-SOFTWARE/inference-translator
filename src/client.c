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
#include <unistd.h>

#ifndef VERSION
#define VERSION "0.1.0"
#endif

static struct termios g_saved_tio;
static bool g_tio_saved;
static const char *g_mount = INF_MOUNT_INFERENCE;

static char g_buf[INF_PROMPT_MAX];      /* prompt being composed */
static size_t g_blen;

static void
restore_tio (void)
{
    if (g_tio_saved)
        tcsetattr (STDIN_FILENO, TCSANOW, &g_saved_tio);
}

static void
on_signal (int sig)
{
    (void) sig;
    restore_tio ();
    _exit (0);
}

/* --- POSIX node access through the mount point --------------------- */

static int
write_node (const char *node, const char *data, size_t len)
{
    char path[4096];
    snprintf (path, sizeof path, "%s/%s", g_mount, node);
    int fd = open (path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    size_t off = 0;
    while (off < len)
      {
        ssize_t n = write (fd, data + off, len - off);
        if (n <= 0)
          {
            close (fd);
            return -1;
          }
        off += (size_t) n;
      }
    if (close (fd) != 0)
        return -1;
    return 0;
}

/* Same, on an explicit mount point (used for /page by /fetch). */
static ssize_t
read_node_m (const char *mount, const char *node, char *out, size_t outsz)
{
    char path[4096];
    snprintf (path, sizeof path, "%s/%s", mount, node);
    int fd = open (path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t total = 0;
    for (;;)
      {
        if ((size_t) total >= outsz - 1)
          {
            /* keep reading to report the full length, discard */
            char sink[512];
            ssize_t n = read (fd, sink, sizeof sink);
            if (n < 0)
                break;
            if (n == 0)
                break;
            total += n;
            continue;
          }
        ssize_t n = read (fd, out + total, outsz - 1 - (size_t) total);
        if (n < 0)
            break;
        if (n == 0)
            break;
        total += n;
      }
    close (fd);
    if (outsz > 0)
        out[total < (ssize_t) outsz - 1 ? total : (ssize_t) outsz - 1] = '\0';
    return total;
}

/* Returns the content length, or -1 when unreadable (absent node,
 * ENODATA before any prompt...). */
static ssize_t
read_node (const char *node, char *out, size_t outsz)
{
    return read_node_m (g_mount, node, out, outsz);
}

/* --- narration ------------------------------------------------------ */

static void
narrate (const char *verb, const char *node)
{
    char line[4200];
    inf_client_narrate (line, sizeof line, verb, g_mount, node);
    if (inf_client_color ())
        printf ("\033[2m→ %s\033[0m\n", line);
    else
        printf ("→ %s\n", line);
}

/* --- commands ------------------------------------------------------- */

static void
cmd_status (void)
{
    char val[256];
    narrate ("read", INF_NODE_STATUS);
    if (read_node (INF_NODE_STATUS, val, sizeof val) < 0)
        printf ("  status illisible (translator non monté ?)\n");
    else
        printf ("  status: %s\n", val);
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

static void
cmd_fetch (const char *arg)
{
    if (arg == NULL || *arg == '\0')
      {
        printf ("  usage: /fetch URL (http://... ou https://...)\n");
        return;
      }
    if (strncmp (arg, "http://", 7) != 0
        && strncmp (arg, "https://", 8) != 0)
      {
        printf ("  URL invalide: http:// ou https:// requis\n");
        return;
      }

    char q[2048];
    char b[2048];
    char cmd[4600];
    shell_quote (q, sizeof q, arg);
    shell_quote (b, sizeof b, find_httpfs ());
    snprintf (cmd, sizeof cmd,
              "settrans -fg " FETCH_MOUNT " 2>/dev/null; "
              "sleep 1; "
              "mkdir -p " FETCH_MOUNT "; "
              "settrans -a " FETCH_MOUNT " %s %s", b, q);
    printf ("→ mount %s %s sur " FETCH_MOUNT "\n", b, arg);
    if (system (cmd) != 0)
      {
        printf ("  échec du montage (settrans + httpfs requis, root, Hurd)\n");
        return;
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
        return;
      }
    printf ("  status HTTP: %ld\n", status);
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

    char *body = malloc (FETCH_MAX_BODY + 1);
    if (body == nullptr)
      {
        printf ("  ENOMEM\n");
        return;
      }
    ssize_t total = read_node_m (FETCH_MOUNT, "content", body,
                                 FETCH_MAX_BODY + 1);
    if (total < 0)
      {
        printf ("  lecture de " FETCH_MOUNT "/content impossible\n");
        free (body);
        return;
      }
    size_t show = total < FETCH_PREVIEW ? (size_t) total : FETCH_PREVIEW;
    printf ("  page: %zd octets (aperçu %zu) — complète dans "
            FETCH_MOUNT "/content\n", total, show);
    fwrite (body, 1, show, stdout);
    printf ("\n");
    free (body);
}

/* /stats - real-time CPU/memory/IO display: redraw every second,
   any key leaves the monitor. */
static void
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

static void
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
}

static void
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

static void
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

    char reqjson[INF_JSON_MAX];
    narrate ("read", INF_NODE_REQUEST);
    if (read_node (INF_NODE_REQUEST, reqjson, sizeof reqjson) < 0)
        printf ("  request illisible avant soumission\n");
    else
        printf ("  request: %s\n", reqjson);

    char val[256];
    narrate ("read", INF_NODE_STATUS);
    if (read_node (INF_NODE_STATUS, val, sizeof val) >= 0)
        printf ("  status: %s\n", val);

    g_blen = 0;
    g_buf[0] = '\0';
}

static void
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

/* --- input loop ------------------------------------------------------ */

/* Redraw the prompt and the current line (after a completion). */
static void
redraw_line (const char *line)
{
    if (inf_client_color ())
        printf ("\r\033[1;32m> \033[0m%s\033[K", line);
    else
        printf ("\r> %s", line);
    fflush (stdout);
}

/* Read one line in raw mode.  Returns false on EOF/quit.
 * Handles DEL/backspace, CSI escape sequences (left arrow erases),
 * Tab completion and a leading backslash (escapes the '/', so the
 * line is plain text: *escaped is set). */
static bool
read_line (char *line, size_t linesz, size_t *outlen, bool *escaped)
{
    size_t len = 0;
    for (;;)
      {
        char c;
        ssize_t n = read (STDIN_FILENO, &c, 1);
        if (n <= 0)
            return false;
        if (c == '\r' || c == '\n')
          {
            line[len] = '\0';
            *outlen = len;
            printf ("\n");     /* ONLCR is on: this emits CR LF */
            return true;
          }
        if (c == 0x03)                   /* Ctrl-C */
            return false;
        if (c == 0x04 && len == 0)      /* Ctrl-D on an empty line */
            return false;
        if (c == '\\' && len == 0)      /* escape a leading '/' */
          {
            if (escaped != NULL)
                *escaped = true;
            continue;
          }
        if (c == 0x1b)                   /* ESC: CSI sequence (arrows, Del) */
          {
            char c2;
            if (read (STDIN_FILENO, &c2, 1) <= 0)
                return false;
            if (c2 == '[')
              {
                char c3;
                for (;;)
                  {
                    if (read (STDIN_FILENO, &c3, 1) <= 0)
                        return false;
                    if (c3 >= 0x40 && c3 <= 0x7e)
                        break;      /* final byte of the CSI sequence */
                  }
                if (c3 == 'D' && len > 0)   /* left arrow: erase */
                  {
                    len--;
                    if (inf_client_color ())
                        printf ("\b \b");
                  }
              }
            continue;
          }
        if (c == '\t')                   /* Tab: completion */
          {
            char comp[INF_PROMPT_MAX];
            const char *list[16];
            size_t nm = inf_client_complete (line, len, comp, sizeof comp,
                                             list, 16);
            if (nm == 1)
              {
                len = strlen (comp);
                if (len >= linesz)
                    len = linesz - 1;
                memcpy (line, comp, len);
                line[len] = '\0';
                redraw_line (line);
                continue;
              }
            if (nm > 1)
              {
                line[len] = '\0';
                printf ("\n");         /* ONLCR emits CR LF */
                for (size_t i = 0; i < nm && i < 16; i++)
                    printf ("%s  ", list[i]);
                printf ("\n");
                redraw_line (line);
                continue;
              }
            if (write (STDOUT_FILENO, "\a", 1) != 1)
                return false;
            continue;
          }
        if ((c == 0x7f || c == 0x08) && len > 0)
          {
            len--;
            if (inf_client_color ())
                printf ("\b \b");
            continue;
          }
        if (len + 1 < linesz)
          {
            line[len++] = c;
            if (write (STDOUT_FILENO, &c, 1) != 1)
                return false;
          }
      }
}

static void
usage (FILE *out)
{
    fprintf (out,
             "Usage: inference [--mount DIR] [--version] [--help]\n"
             "Client interactif de /inference (par défaut, ou $INFERENCE_MOUNT).\n"
             "Commandes: /editor /task TÂCHE /submit /fetch URL /status /stats /help /quit.\n"
             "Édition: DEL efface, Tab complète, '\\' échappe un '/' initial.\n");
}

int
main (int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
      {
        if (strcmp (argv[i], "--mount") == 0 || strcmp (argv[i], "-m") == 0)
          {
            if (++i >= argc)
              {
                usage (stderr);
                return 2;
              }
            g_mount = argv[i];
          }
        else if (strcmp (argv[i], "--version") == 0)
          {
            printf ("inference (GNU AI) %s\n", VERSION);
            return 0;
          }
        else if (strcmp (argv[i], "--help") == 0)
          {
            usage (stdout);
            return 0;
          }
        else
          {
            fprintf (stderr, "inference: option inconnue: %s\n", argv[i]);
            usage (stderr);
            return 2;
          }
      }
    const char *env = getenv ("INFERENCE_MOUNT");
    if (env != NULL && *env != '\0' && argc == 1)
        g_mount = env;

    /* SPEC.md section 6: the interactive mode refuses to start
     * without a terminal, with an explicit message. */
    if (!isatty (STDIN_FILENO) || !isatty (STDOUT_FILENO))
      {
        fprintf (stderr,
                 "inference: le mode interactif exige un terminal "
                 "(stdin et stdout doivent être des TTY)\n");
        return 2;
      }

    struct termios tio;
    if (tcgetattr (STDIN_FILENO, &tio) != 0)
        return 2;
    g_saved_tio = tio;
    g_tio_saved = true;
    atexit (restore_tio);
    signal (SIGINT, on_signal);
    signal (SIGTERM, on_signal);
    signal (SIGHUP, on_signal);

    char banner[256];
    inf_client_banner (banner, sizeof banner, VERSION);
    fputs (banner, stdout);
    cmd_status ();

    /* POSIX cbreak: disable the line discipline and the echo, keep
       OPOST|ONLCR so '\n' in the output still emits CR LF (cfmakeraw
       would clear OPOST and the display would staircase). */
    tio.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    tio.c_lflag &= ~(ECHO | ICANON | IEXTEN);
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;
    tcsetattr (STDIN_FILENO, TCSANOW, &tio);

    char line[INF_PROMPT_MAX];
    for (;;)
      {
        if (inf_client_color ())
            printf ("\033[1;32m> \033[0m");
        else
            printf ("> ");
        fflush (stdout);

        size_t llen;
        bool escaped = false;
        if (!read_line (line, sizeof line, &llen, &escaped))
            break;

        char arg[256] = "";
        enum inf_client_cmd cmd = escaped ? INF_CMD_NONE
            : inf_client_parse_command (line, arg, sizeof arg);
        if (cmd == INF_CMD_NONE)
          {
            /* plain text: append to the prompt buffer, multi-line */
            if (g_blen + llen + 1 < INF_PROMPT_MAX)
              {
                memcpy (g_buf + g_blen, line, llen);
                g_blen += llen;
                g_buf[g_blen++] = '\n';
                g_buf[g_blen] = '\0';
              }
            continue;
          }
        switch (cmd)
          {
          case INF_CMD_QUIT:
            restore_tio ();
            return 0;
          case INF_CMD_STATUS:
            cmd_status ();
            break;
          case INF_CMD_FETCH:
            cmd_fetch (arg);
            break;
          case INF_CMD_STATS:
            cmd_stats ();
            break;
          case INF_CMD_HELP:
            cmd_help ();
            break;
          case INF_CMD_TASK:
            cmd_task (arg);
            break;
          case INF_CMD_SUBMIT:
            cmd_submit ();
            break;
          case INF_CMD_EDITOR:
            cmd_editor ();
            break;
          case INF_CMD_UNKNOWN:
          default:
            printf ("  commande inconnue: %s\n", line);
            break;
          }
      }

    restore_tio ();
    return 0;
}
