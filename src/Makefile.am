# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

bin_PROGRAMS = inference-translator

# --- Phase 0: the compilable skeleton --------------------------------
#
# The pure core (frozen contracts of SPEC.md: contract.c, expr.c,
# actions.c, spinner.c) has no external dependency and builds as a
# convenience library shared by the binary and the contract tests.
# The translator /inference (prompt -> structured request) arrives
# with phase 1 on trivfs; the interactive client with phase 2.

noinst_LIBRARIES = libinfcore.a
libinfcore_a_SOURCES = contract.c expr.c actions.c spinner.c

inference_translator_SOURCES = main.c
inference_translator_LDADD = libinfcore.a

# The ncurses --cli prototype is built only when configure found
# ncurses (HAVE_NCURSES); the core stays buildable with the GNU
# toolchain alone (PLAN.md section 4).
if HAVE_NCURSES
inference_translator_SOURCES += cli.c
endif

AM_CPPFLAGS = -I$(top_srcdir) -I$(top_srcdir)/include
AM_CFLAGS = $(C23_CFLAGS) -Wall -Wextra
