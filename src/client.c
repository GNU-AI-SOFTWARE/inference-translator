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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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

struct termios g_saved_tio;
bool g_tio_saved;
const char *g_mount = INF_MOUNT_INFERENCE;
const char *g_db = "/db";        /* --db DIR / $INFERENCE_DB */
const char *g_orch = INF_MOUNT_ORCHESTRATE; /* --orchestrate DIR */
const char *g_training_cache = NULL; /* $INFERENCE_TRAINING / $HOME/.inference-training.ndjson */
struct inf_history g_hist;               /* --history FILE */
char g_hist_path[4096] = "";
char g_hist_saved[INF_PROMPT_MAX];       /* in-progress line while browsing */
size_t g_hpos;                           /* == g_hist.count: editing a new line */
time_t g_start;                          /* client session start (uptime) */

char g_buf[INF_PROMPT_MAX];      /* prompt being composed */
size_t g_blen;
/* input drained before the raw switch, served first by read_line */
char g_in_stash[4096];
size_t g_in_stash_len;
size_t g_in_stash_pos;
void
restore_tio (void)
{
    if (g_tio_saved)
        tcsetattr (STDIN_FILENO, TCSANOW, &g_saved_tio);
}

static void
inf_history_free_atexit (void)
{
    inf_history_free (&g_hist);
}

void
on_signal (int sig)
{
    (void) sig;
    restore_tio ();
    _exit (0);
}

/* --- POSIX node access through the mount point --------------------- */

static void
redraw_line (const char *line)
{
    if (inf_client_color ())
      {
        char hl[INF_PROMPT_MAX * 4];
        inf_text_highlight (hl, sizeof hl, line, true);
        printf ("\r\033[1;32m> \033[0m%s\033[K", hl);
      }
    else
        printf ("\r> %s", line);
    fflush (stdout);
}

/* Erase one full character (UTF-8 aware: continuation bytes
 * 0x80..0xBF belong to the previous lead byte) from the line buffer.
 * Returns the new length. */
static size_t
del_char (char *line, size_t len)
{
    if (len == 0)
        return 0;
    len--;
    while (len > 0 && ((unsigned char) line[len] & 0xC0) == 0x80)
        len--;
    return len;
}

/* Visual erase of the last character: redraw the whole (highlighted)
 * line in color mode - atomic, no staircase - or "\b \b" per cell
 * otherwise. */
static void
erase_last_char (char *line, size_t len)
{
    line[len] = '\0';
    if (inf_client_color ())
        redraw_line (line);
    else
        printf ("\b \b");
    fflush (stdout);
}

/* One input byte: the pre-raw stash first, then the tty. */
static ssize_t
read_byte (char *c)
{
    if (g_in_stash_pos < g_in_stash_len)
      {
        *c = g_in_stash[g_in_stash_pos++];
        return 1;
      }
    return read (STDIN_FILENO, c, 1);
}

/* Read one line in raw mode.  Returns false on EOF/quit.
 * Handles DEL/backspace, CSI escape sequences (left arrow erases),
 * Tab completion and a leading backslash (escapes the '/', so the
 * line is plain text: *escaped is set). */
bool
read_line (char *line, size_t linesz, size_t *outlen, bool *escaped)
{
    size_t len = 0;
    for (;;)
      {
        char c;
        ssize_t n = read_byte (&c);
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
            if (read_byte (&c2) <= 0)
                return false;
            if (c2 == '[')
              {
                char c3;
                for (;;)
                  {
                    if (read_byte (&c3) <= 0)
                        return false;
                    if (c3 >= 0x40 && c3 <= 0x7e)
                        break;      /* final byte of the CSI sequence */
                  }
                if (c3 == 'D' && len > 0)   /* left arrow: erase */
                  {
                    len = del_char (line, len);
                    erase_last_char (line, len);
                  }
                else if (c3 == 'A' && g_hist.count > 0)     /* up: older */
                  {
                    if (g_hpos == g_hist.count)
                      {
                        memcpy (g_hist_saved, line, len + 1);
                        g_hist_saved[len] = '\0';
                      }
                    if (g_hpos > 0)
                      g_hpos--;
                    const char *h = inf_history_get (&g_hist, g_hpos);
                    if (h != NULL)
                      {
                        len = strlen (h);
                        if (len >= linesz)
                            len = linesz - 1;
                        memcpy (line, h, len);
                        line[len] = '\0';
                        redraw_line (line);
                      }
                  }
                else if (c3 == 'B' && g_hpos < g_hist.count) /* down: newer */
                  {
                    g_hpos++;
                    const char *h = g_hpos == g_hist.count
                        ? g_hist_saved : inf_history_get (&g_hist, g_hpos);
                    len = strlen (h);
                    if (len >= linesz)
                        len = linesz - 1;
                    memcpy (line, h, len);
                    line[len] = '\0';
                    redraw_line (line);
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
            len = del_char (line, len);
            erase_last_char (line, len);
            continue;
          }
        if (len + 1 < linesz)
          {
            line[len++] = c;
            line[len] = '\0';
            if (inf_client_color ())
              redraw_line (line);       /* live highlight as we type */
            else if (write (STDOUT_FILENO, &c, 1) != 1)
                return false;
          }
      }
}
static void
usage (FILE *out)
{
    fprintf (out,
             "Usage: inference [--mount DIR] [--db DIR] [--orchestrate DIR] [--history FILE] [--batch [FILE|-]] [--version] [--help]\n"
             "--batch : mode non interactif (pas de TTY requis) — lit des commandes ligne par ligne dans FILE (ou stdin avec -).\n"
             "Client interactif de /inference (par défaut, ou $INFERENCE_MOUNT).\n"
             "Commandes: /editor /task TÂCHE /submit /fetch URL /learn [URL...] /read-code URL /learn-code URL /watch /history /uptime /reply [question] /good /bad /sigmoid /entrainement URL|hf://... [DIR] /entrainement --from FILE|--from-feedback /hf sync hf://... DIR /db status|search|list|count /status /stats /help /quit.\n"
             "Base de données: /db par défaut (--db DIR ou $INFERENCE_DB).\n"
             "Orchestrateur: /orchestrate par défaut (--orchestrate DIR ou $INFERENCE_ORCH).\n"
             "Cache d'entraînement (pour /reply): $INFERENCE_TRAINING ou $HOME/.inference-training.ndjson.\n"
             "Historique: $HOME/.history par défaut (--history FILE ou $INFERENCE_HISTORY),\n    flèche haut/bas pour rappeler une ligne.\n"
             "Édition: DEL efface, Tab complète, '\\' échappe un '/' initial.\n");
}

static bool g_quit;

/* handle_line: one input line -> dispatch (shared by the REPL and
   --batch mode). */
static void
handle_line (const char *line, size_t llen, bool escaped)
{
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
            return;
          }
        switch (cmd)
          {
          case INF_CMD_QUIT:
            g_quit = true;
            break;
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
          case INF_CMD_LEARN:
            cmd_learn (arg);
            break;
          case INF_CMD_READ_CODE:
            cmd_read_code (arg);
            break;
          case INF_CMD_LEARN_CODE:
            cmd_learn_code (arg);
            break;
          case INF_CMD_WATCH:
            cmd_watch ();
            break;
          case INF_CMD_HISTORY:
            cmd_history ();
            break;
          case INF_CMD_UPTIME:
            cmd_uptime ();
            break;
          case INF_CMD_REPLY:
            cmd_reply (arg);
            break;
          case INF_CMD_SIGMOID:
            cmd_sigmoid ();
            break;
          case INF_CMD_TRAINING:
            cmd_training (arg);
            break;
          case INF_CMD_HF:
            cmd_hf (arg);
            break;
          case INF_CMD_DB:
            cmd_db (arg);
            break;
          case INF_CMD_CODE:
            cmd_code (arg);
            break;
          case INF_CMD_GOOD:
            cmd_feedback ("good");
            break;
          case INF_CMD_BAD:
            cmd_feedback ("bad");
            break;
          case INF_CMD_UNKNOWN:
          default:
            printf ("  commande inconnue: %s\n", line);
            break;
          }
}

static const char *g_batch = NULL;

int
main (int argc, char **argv)
{
    /* env first, so an explicit --mount/--db on the command line wins */
    const char *env0 = getenv ("INFERENCE_MOUNT");
    if (env0 != NULL && *env0 != '\0')
        g_mount = env0;
    env0 = getenv ("INFERENCE_DB");
    if (env0 != NULL && *env0 != '\0')
        g_db = env0;
    env0 = getenv ("INFERENCE_ORCH");
    if (env0 != NULL && *env0 != '\0')
        g_orch = env0;
    env0 = getenv ("INFERENCE_TRAINING");
    if (env0 != NULL && *env0 != '\0')
        g_training_cache = env0;
    else
      {
        const char *home = getenv ("HOME");
        static char defcache[4096];
        if (home != NULL && *home != '\0')
            snprintf (defcache, sizeof defcache,
                      "%s/.inference-training.ndjson", home);
        else
            snprintf (defcache, sizeof defcache,
                      ".inference-training.ndjson");
        g_training_cache = defcache;
      }
    env0 = getenv ("INFERENCE_HISTORY");
    if (env0 != NULL && *env0 != '\0')
        snprintf (g_hist_path, sizeof g_hist_path, "%s", env0);
    else
      {
        const char *home = getenv ("HOME");
        if (home != NULL && *home != '\0')
            snprintf (g_hist_path, sizeof g_hist_path, "%s/.history", home);
        else
            snprintf (g_hist_path, sizeof g_hist_path, ".history");
      }

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
        else if (strcmp (argv[i], "--db") == 0)
          {
            if (++i >= argc)
              {
                usage (stderr);
                return 2;
              }
            g_db = argv[i];
          }
        else if (strcmp (argv[i], "--orchestrate") == 0)
          {
            if (++i >= argc)
              {
                usage (stderr);
                return 2;
              }
            g_orch = argv[i];
          }
        else if (strcmp (argv[i], "--history") == 0)
          {
            if (++i >= argc)
              {
                usage (stderr);
                return 2;
              }
            snprintf (g_hist_path, sizeof g_hist_path, "%s", argv[i]);
          }
        else if (strcmp (argv[i], "--batch") == 0)
          {
            /* optional value: --batch FILE, or --batch (stdin) */
            if (i + 1 < argc && (argv[i + 1][0] != '-' || strcmp (argv[i + 1], "-") == 0))
                g_batch = argv[++i];
            else
                g_batch = "-";
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

    /* --batch: no TTY needed; read command lines from FILE ("-" = stdin)
       and dispatch them, for scripted ingestion (/entrainement lists). */
    if (g_batch != NULL)
      {
        FILE *in = stdin;
        if (strcmp (g_batch, "-") != 0)
          {
            in = fopen (g_batch, "r");
            if (in == NULL)
              {
                fprintf (stderr, "inference: impossible d'ouvrir %s\n", g_batch);
                perror ("");
                return 2;
              }
          }
        char banner[256];
        inf_client_banner (banner, sizeof banner, VERSION);
        fputs (banner, stdout);
        cmd_status ();
        char line[INF_PROMPT_MAX];
        while (fgets (line, sizeof line, in) != NULL)
          {
            size_t llen = strlen (line);
            while (llen > 0 && (line[llen - 1] == '\n' || line[llen - 1] == '\r'))
                line[--llen] = '\0';
            if (llen == 0)
                continue;
            handle_line (line, llen, false);
            fflush (stdout);
            if (g_quit)
                break;
          }
        if (in != stdin)
            fclose (in);
        return 0;
      }

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

    g_start = time (NULL);
    inf_history_init (&g_hist);
    inf_history_load (&g_hist, g_hist_path);
    g_hpos = g_hist.count;
    atexit (inf_history_free_atexit);

    char banner[256];
    inf_client_banner (banner, sizeof banner, VERSION);
    fputs (banner, stdout);
    cmd_status ();

    /* Drain the input that already arrived before the raw switch: pty
       servers that flush the canonical queue when ICANON is cleared
       (GNU/Hurd) would otherwise eat it.  In canonical mode FIONREAD
       counts only complete lines and read() returns them without
       blocking, so this never waits for the user. */
    {
      int pending = 0;
      if (ioctl (STDIN_FILENO, FIONREAD, &pending) == 0)
        while (pending > 0 && g_in_stash_len < sizeof g_in_stash)
          {
            char b[256];
            ssize_t r = read (STDIN_FILENO, b,
                              (size_t) (pending > 256 ? 256 : pending));
            if (r <= 0)
              break;
            memcpy (g_in_stash + g_in_stash_len, b, (size_t) r);
            g_in_stash_len += (size_t) r;
            pending -= (int) r;
          }
    }

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
        /* persistent info line: machine usage, between the prompt line
           and the rest of the window */
        char sline[256];
        inf_client_stats_line (sline, sizeof sline);
        if (inf_client_color ())
            printf ("\033[2m  %s\033[0m\n", sline);
        else
            printf ("  %s\n", sline);
        if (inf_client_color ())
            printf ("\033[1;32m> \033[0m");
        else
            printf ("> ");
        fflush (stdout);

        size_t llen;
        bool escaped = false;
        if (!read_line (line, sizeof line, &llen, &escaped))
            break;
        if (llen > 0)
          {
            inf_history_add (&g_hist, line);
            (void) inf_history_append (g_hist_path, line);
            g_hpos = g_hist.count;
          }

        handle_line (line, llen, escaped);
        if (g_quit)
          {
            restore_tio ();
            return 0;
          }
      }

    restore_tio ();
    return 0;
}
