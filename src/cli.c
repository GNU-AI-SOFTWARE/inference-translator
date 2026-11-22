/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * cli.c -- ncurses prototype of the --cli interface (phase 0).
 *
 * Built only when configure found ncurses (HAVE_NCURSES); the core
 * translator stays buildable with the GNU toolchain alone (PLAN.md
 * section 4).  Layout (SPEC.md section 8): banner, main window with
 * the Actions and Expressions panes, the narration log and the last
 * request JSON, an animated hourglass while submitting, and the ">"
 * prompt line at the bottom.  NO_COLOR disables colors; without a
 * TTY the command fails with an explicit message (tests/test_cli.sh).
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifndef VERSION
# ifdef PACKAGE_VERSION
#  define VERSION PACKAGE_VERSION
# else
#  define VERSION "0.1.0"
# endif
#endif

#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ncurses.h>

#include "inference.h"

#define INF_PROMPT_MAX      4096
#define INF_LOG_LINES       3
#define INF_SPINNER_DELAY_MS 120

static char g_prompt[INF_PROMPT_MAX];
static char g_log[INF_LOG_LINES][256];
static char g_request[INF_JSON_MAX];

static bool
colors_disabled (void)
{
    const char *v = getenv ("NO_COLOR");
    return v != NULL && v[0] != '\0';
}

/* Print one line, truncated to the remaining terminal width so a
 * long JSON request never makes mvprintw () fail at the edge. */
static void
put_line (int row, int col, const char *text, bool color, int pair)
{
    int rows, cols;
    getmaxyx (stdscr, rows, cols);
    if (row < 0 || row >= rows || col < 0 || col >= cols)
      return;
    int width = cols - col;
    char tmp[INF_JSON_MAX];
    if (width > 0 && (int) strlen (text) >= width)
      {
        size_t n = (size_t) (width - 1);
        if (n >= sizeof tmp)
          n = sizeof tmp - 1;
        memcpy (tmp, text, n);
        tmp[n] = '\0';
        text = tmp;
      }
    if (color)
      attron (COLOR_PAIR (pair));
    mvprintw (row, col, "%s", text);
    if (color)
      attroff (COLOR_PAIR (pair));
}

static void
narrate (const char *msg)
{
    for (int i = 0; i < INF_LOG_LINES - 1; i++)
      {
        strncpy (g_log[i], g_log[i + 1], sizeof g_log[i] - 1);
        g_log[i][sizeof g_log[i] - 1] = '\0';
      }
    strncpy (g_log[INF_LOG_LINES - 1], msg, sizeof g_log[INF_LOG_LINES - 1] - 1);
    g_log[INF_LOG_LINES - 1][sizeof g_log[INF_LOG_LINES - 1] - 1] = '\0';
}

/* Redraw the whole main window: Actions, Expressions, narration log
 * and the last request JSON.  The spinner lives at the top right. */
static void
draw_main (bool color)
{
    int rows, cols;
    getmaxyx (stdscr, rows, cols);
    int y = 2;
    int log_y = 0;

    put_line (y++, 2, "Actions:", color, 1);
    for (size_t i = 0; i < inf_actions_count && y < rows - 2; i++, y++)
      {
        char line[320];
        snprintf (line, sizeof line, "  %-8s %s",
                  inf_actions[i].command, inf_actions[i].help);
        put_line (y, 2, line, false, 0);
      }

    y++;
    put_line (y++, 2, "Expressions:", color, 1);
    for (size_t i = 0; i < inf_exprs_count && y < rows - 2; i++, y++)
      {
        char line[INF_URL_LEN + 32];
        snprintf (line, sizeof line, "  %s = %s",
                  inf_exprs[i].name, inf_exprs[i].pattern);
        put_line (y, 2, line, false, 0);
      }

    y++;
    put_line (y, 2, "Log:", color, 1);
    log_y = y + 1;
    for (int i = 0; i < INF_LOG_LINES && log_y + i < rows - 2; i++)
      put_line (log_y + i, 2, g_log[i], false, 0);

    int req_y = rows - 2;
    if (g_request[0] != '\0')
      put_line (req_y, 2, g_request, color, 2);
    (void) cols;
}

static void
draw_spinner (int frame, bool color, bool erase)
{
    int rows, cols;
    getmaxyx (stdscr, rows, cols);
    (void) rows;
    int base_c = cols - INF_SPINNER_SIZE - 1;
    if (base_c < 40)
      base_c = 40;
    for (int r = 0; r < INF_SPINNER_SIZE; r++)
      {
        if (color)
          attron (COLOR_PAIR (2));
        mvprintw (2 + r, base_c, "%s",
                  erase ? "......." : inf_spinner[frame][r]);
        if (color)
          attroff (COLOR_PAIR (2));
      }
}

/* Animate the hourglass for two full loops, or until q/Esc. */
static void
animate_spinner (bool color)
{
    timeout (INF_SPINNER_DELAY_MS);
    int shown = 0;
    int frame = 0;
    while (shown < 2 * INF_SPINNER_FRAMES)
      {
        draw_spinner (frame, color, false);
        refresh ();
        int ch = getch ();
        if (ch == 'q' || ch == 27)
          break;
        frame = (frame + 1) % INF_SPINNER_FRAMES;
        shown++;
      }
    timeout (-1);
    draw_spinner (0, color, true);
}

/* Parse the current prompt into the type 1 request, show it, then
 * spin while a phase 1 translator would submit it. */
static void
do_submit (bool color)
{
    if (g_prompt[0] == '\0')
      {
        narrate ("prompt vide : rien à soumettre");
        draw_main (color);
        return;
      }
    struct inf_request req;
    inf_parse_prompt (g_prompt, &req);
    inf_request_to_json (&req, g_request, sizeof g_request);
    char msg[256];
    snprintf (msg, sizeof msg,
              "j'extrais %zu URL(s), tâche détectée : %s",
              req.url_count, inf_task_name (req.task));
    narrate (msg);
    narrate ("je soumets la requête à l'orchestrateur (/orchestrate)");
    draw_main (color);
    animate_spinner (color);
    narrate ("requête soumise (translator /inference : phase 1)");
    draw_main (color);
}

/* Dispatch a slash command; return true when the CLI must quit. */
static bool
handle_action (const char *cmd, bool color)
{
    for (size_t i = 0; i < inf_actions_count; i++)
        if (strcmp (cmd, inf_actions[i].command) == 0)
          {
            if (strcmp (cmd, "/quit") == 0)
              return true;
            if (strcmp (cmd, "/submit") == 0)
              do_submit (color);
            else if (strcmp (cmd, "/editor") == 0)
              narrate ("éditeur : $VISUAL -> $EDITOR -> nano -> vi"
                       " (SPEC.md section 5, phase 2)");
            else if (strcmp (cmd, "/task") == 0)
              narrate ("surcharge de tâche via /inference/task (phase 1)");
            else if (strcmp (cmd, "/status") == 0)
              {
                char msg[128];
                snprintf (msg, sizeof msg, "status : %s",
                          inf_status_name (INF_STATUS_IDLE));
                narrate (msg);
              }
            draw_main (color);
            return false;
          }
    narrate ("commande inconnue (voir Actions)");
    draw_main (color);
    return false;
}

int
inf_cli_run (void)
{
    /* Without a terminal there is nothing to draw: fail explicitly
     * instead of letting ncurses die on a pipe (tests/test_cli.sh). */
    if (!isatty (STDIN_FILENO) || !isatty (STDOUT_FILENO))
      {
        fprintf (stderr,
                 "inference-translator: --cli needs an interactive terminal\n");
        return EXIT_FAILURE;
      }

    setlocale (LC_ALL, "");
    bool color = !colors_disabled ();

    initscr ();
    cbreak ();
    noecho ();
    keypad (stdscr, TRUE);
    curs_set (0);
    if (color && has_colors ())
      {
        start_color ();
        init_pair (1, COLOR_CYAN, COLOR_BLACK);
        init_pair (2, COLOR_YELLOW, COLOR_BLACK);
      }
    else
      color = false;

    g_prompt[0] = '\0';
    g_request[0] = '\0';
    for (int i = 0; i < INF_LOG_LINES; i++)
      g_log[i][0] = '\0';
    narrate ("phase 0 : contrats gelés (SPEC.md), /inference arrive en phase 1");

    put_line (0, 0, "", false, 0);
    if (color)
      attron (COLOR_PAIR (1) | A_BOLD);
    mvprintw (0, 0, " inference-translator (GNU AI) %s -- --cli", VERSION);
    if (color)
      attroff (COLOR_PAIR (1) | A_BOLD);
    draw_main (color);
    refresh ();

    bool quit = false;
    while (!quit)
      {
        int rows, cols;
        getmaxyx (stdscr, rows, cols);
        (void) cols;
        move (rows - 1, 0);
        addstr ("> ");
        clrtoeol ();
        refresh ();
        char buf[INF_PROMPT_MAX];
        if (wgetnstr (stdscr, buf, (int) sizeof buf - 1) == ERR)
          break;
        if (buf[0] == '\0')
          continue;
        if (buf[0] == '/')
          quit = handle_action (buf, color);
        else
          {
            strncpy (g_prompt, buf, sizeof g_prompt - 1);
            g_prompt[sizeof g_prompt - 1] = '\0';
            do_submit (color);
          }
      }

    endwin ();
    return EXIT_SUCCESS;
}
