# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

AM_CPPFLAGS = -I$(top_srcdir) -I$(top_srcdir)/include

bin_PROGRAMS = inference-translator inference

# --- The portable core -------------------------------------------------
#
# contract.c (frozen contracts of SPEC.md), expr.c, actions.c,
# spinner.c and fs.c (the /inference node engine of phase 1) are pure
# C23/POSIX with no external dependency.  The library is shared by the
# binary and the test suite.
noinst_LIBRARIES = libinfcore.a
libinfcore_a_SOURCES = contract.c expr.c actions.c spinner.c fs.c \
    client-core.c training.c text.c stats.c jsonlite.c history.c neuron.c rootmode.c

# --- The phase 2 client `inference` ---------------------------------
#
# Pure POSIX (termios + ANSI, no TUI library): it talks to the mount
# point through plain open/read/write and works on any host, Hurd or
# not (the tests point it at a fake tree with --mount).
inference_SOURCES = client.c nodeio.c cmds.c
inference_CPPFLAGS = $(AM_CPPFLAGS)
inference_LDADD = libinfcore.a -lm

# /stats uses Mach host statistics on Hurd (/proc elsewhere).
if ON_HURD
inference_CPPFLAGS += -DON_HURD
inference_LDADD += -lmachuser -lpthread
endif

noinst_HEADERS = translator.h client_internal.h

# --- The binary face, per host -----------------------------------------
#
# On GNU/Hurd, main-hurd.c follows the canonical libnetfs translator
# structure (netfs_startup then netfs_server_loop) and netfs-hooks.c
# maps the RPCs onto fs.c; the binary is the /inference translator.
# On any other host, main.c builds the portable verification binary
# (GNU base commands, --cli prototype, phase message).
if ON_HURD
inference_translator_SOURCES = main-hurd.c netfs-hooks.c
inference_translator_CPPFLAGS = $(AM_CPPFLAGS) -DON_HURD
inference_translator_LDADD = libinfcore.a -lnetfs -lfshelp -lports \
                             -lihash -liohelp -lmachuser -lpthread \
                             -lshouldbeinlibc
else
inference_translator_SOURCES = main.c
inference_translator_CPPFLAGS = $(AM_CPPFLAGS)
inference_translator_LDADD = libinfcore.a -lm
endif

# The ncurses --cli prototype is built only when configure found
# ncurses (HAVE_NCURSES); the core stays buildable with the GNU
# toolchain alone (PLAN.md section 4).
if HAVE_NCURSES
inference_translator_SOURCES += cli.c
endif

AM_CFLAGS = $(C23_CFLAGS) -Wall -Wextra
