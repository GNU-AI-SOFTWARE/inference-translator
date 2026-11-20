/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * main-hurd.c -- Entry point of the /inference translator on GNU/Hurd
 * (phase 1 MVP).
 *
 * USAGE
 * -----
 *      settrans -a /inference inference-translator
 *      echo "résume https://example.org" | tee /inference/prompt
 *      cat /inference/request
 *
 * Canonical libnetfs translator structure (same as
 * httpfs-translator): netfs_init (), build the root node, answer
 * settrans with netfs_startup (), then netfs_server_loop ().  A
 * translator that returns from main () dies and settrans reports
 * "Translator died".
 *
 * The GNU base commands (--version/--help) and the --cli prototype
 * are still answered when the binary is run directly, so the
 * translator stays inspectable like any other GNU tool.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifndef VERSION
#define VERSION "0.1.0"
#endif

#if ON_HURD == 1

#include <mach/port.h>                  /* mach_port_t */
typedef mach_port_t *processor_name_array_t;

#include <hurd.h>
#include <hurd/netfs.h>

#include <error.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inference.h"
#include "translator.h"

static struct inf_fs fs;

/* --- The GNU base commands (same answers as the portable main.c) -- */

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
    printf ("\n");
    printf ("As a translator (GNU/Hurd):\n");
    printf ("  settrans -a /inference inference-translator\n");
    printf ("Then a prompt is plain POSIX:\n");
    printf ("  echo \"résumé https://example.org\" | tee /inference/prompt\n");
    printf ("  cat /inference/request\n");
    printf ("\n");
    printf ("Report bugs at"
            " <https://github.com/GNU-AI-SOFTWARE/inference-translator/issues>.\n");
}

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
    if (handle_gnu_options (argc, argv))
        return EXIT_SUCCESS;

    for (int i = 1; i < argc; i++)
        if (strcmp (argv[i], "--cli") == 0)
          {
#ifdef HAVE_NCURSES
            return inf_cli_run ();
#else
            fprintf (stderr, "inference-translator: --cli is not in"
                     " this build: configure did not find ncurses"
                     " (HAVE_NCURSES)\n");
            return EXIT_FAILURE;
#endif
          }

    /* No arguments: run as the /inference translator. */
    netfs_init ();
    inf_fs_init (&fs);
    inf_translator_init (&fs);

    mach_port_t bootstrap;
    task_get_bootstrap_port (mach_task_self (), &bootstrap);
    if (bootstrap == MACH_PORT_NULL)
        error (1, 0, "inference-translator: must be started as a translator"
               " (settrans -a /inference inference-translator)");

    /* Reply to settrans, then serve RPCs forever (never returns). */
    netfs_startup (bootstrap, 0);
    netfs_server_loop ();
    return 0;
}

#endif /* ON_HURD == 1 */
