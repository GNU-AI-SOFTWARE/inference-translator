# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

# The contract tests link the pure core only -- no ncurses, no TTY:
# they run on any POSIX system, Hurd or not.  The CLI conformance test
# needs nothing but the built binary.
TESTS = test_cli.sh test_contract
check_PROGRAMS = test_contract

test_contract_SOURCES = test_contract.c
test_contract_LDADD = $(top_builddir)/src/libinfcore.a
test_contract_CPPFLAGS = -I$(top_srcdir)/include

EXTRA_DIST = test_cli.sh
