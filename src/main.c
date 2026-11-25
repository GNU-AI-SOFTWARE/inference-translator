/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * main.c — Entry point of the inference-translator (phase 0 skeleton).
 *
 * USAGE
 * -----
 *      inference-translator --version
 *      inference-translator --help
 *      settrans -a /inference inference-translator      (phase 1)
 *
 * inference-translator is the human-machine interface of the GNU
 * AI stack: it receives the prompts, lets the user compose in any
 * editor ($VISUAL/$EDITOR), and exposes the structured request
 * consumed by orchestrator-translator (the JSON trio
 * request/status/result, frozen in phase 0 — see PLAN.md).
 *
 * The phase 0 skeleton answers the GNU base commands and refuses
 * everything else: the /inference translator itself arrives with
 * phase 1.  The build architecture is the one shared by the whole
 * stack (see httpfs-translator), so the phases plug in without
 * touching configure.ac again.
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

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "inference.h"

/* --- The GNU base commands ----------------------------------------
   Every binary of the GNU AI stack answers --version and --help
   before anything else, so a translator stays inspectable like any
   other GNU tool — from a shell or from a script.  The answers
   follow the GNU coding standards; the version number comes from
   configure (config.h), the single source of truth of the release. */

static void
print_version (void)
{
    printf ("inference-translator (GNU AI) %s\n", VERSION);
    printf ("License GPLv3+: GNU GPL version 3 or later"
            " <https://gnu.org/licenses/gpl.html>.\n");
    printf ("This is free software: you are free to change"
            " and redistribute it.\n");
    printf ("There is NO WARRANTY, to the extent permitted by law.\n");
}

static void
print_help (void)
{
    printf ("Usage: inference-translator [OPTION]...\n");
    printf ("Human-machine interface of the GNU AI stack for GNU/Hurd.\n");
    printf ("\n");
    printf ("  -h, --help     display this help and exit\n");
    printf ("  -V, --version  output version information and exit\n");
#ifdef HAVE_NCURSES
    printf ("      --cli      ncurses interface prototype (needs a terminal)\n");
#else
    printf ("      --cli      ncurses interface prototype"
            " (not in this build: no ncurses)\n");
#endif
    printf ("      --root DIR  portable test mode: serve the /inference node\n"
            "                  engine as plain files under DIR (non-Hurd hosts)\n");
    printf ("\n");
    printf ("Phase 1 mounts the translator:\n");
    printf ("  settrans -a /inference inference-translator\n");
    printf ("Then a prompt is plain POSIX:\n");
    printf ("  echo \"résume https://example.org\" | tee /inference/prompt\n");
    printf ("  cat /inference/request\n");
    printf ("\n");
    printf ("Report bugs at"
            " <https://github.com/gnu-ai/inference-translator/issues>.\n");
}


/* Scan the command line; return true when one of the two base
 * commands was recognized and answered (the caller exits 0),
 * false when the normal startup should proceed. */
static bool
handle_gnu_options (int argc, char *argv[])
{
    for (int i = 1; i < argc; i++)
        {
            if (strcmp (argv[i], "--version") == 0
                || strcmp (argv[i], "-V") == 0)
                {
                    print_version ();
                    return true;
                }
            if (strcmp (argv[i], "--help") == 0
                || strcmp (argv[i], "-h") == 0)
                {
                    print_help ();
                    return true;
                }
        }
    return false;
}

int
main (int argc, char *argv[])
{
    /* The base commands are answered before any Hurd library is
     * initialized. */
    if (handle_gnu_options (argc, argv))
        return EXIT_SUCCESS;

    /* Portable file-backed engine (tests on non-Hurd hosts). */
    for (int i = 1; i < argc; i++)
        if (strcmp (argv[i], "--root") == 0)
          {
            if (++i >= argc)
              {
                fprintf (stderr,
                         "inference-translator: --root needs a directory\n");
                return EXIT_FAILURE;
              }
            return inf_rootmode_serve (argv[i]);
          }

    /* Phase 0 interface prototype: the ncurses CLI, only when the
     * build found ncurses; it refuses cleanly without a TTY. */
    for (int i = 1; i < argc; i++)
        if (strcmp (argv[i], "--cli") == 0)
            {
#ifdef HAVE_NCURSES
                return inf_cli_run ();
#else
                fprintf (stderr, "inference-translator: --cli is not in"
                         " this build: configure did not find ncurses"
                         " (HAVE_NCURSES); the core still works\n");
                return EXIT_FAILURE;
#endif
            }

    /* This portable build (non-Hurd host) has no server loop: refuse
     * cleanly.  On GNU/Hurd the binary is built from main-hurd.c and
     * mounted with settrans. */
    fprintf (stderr, "inference-translator: portable build — on GNU/Hurd,"
             " mount the translator with:\n"
             "  settrans -a /inference inference-translator\n"
             " (see PLAN.md and SPEC.md)\n");
    return EXIT_FAILURE;
}
