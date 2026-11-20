# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

# The contract and engine tests link the pure core only -- no ncurses,
# no Hurd libraries, no TTY: they run on any POSIX system, Hurd or
# not.  test_posix.sh drives the really mounted translator and skips
# (exit 77) when /inference is absent.  test_cli.sh needs only the
# built binary.
TESTS = test_cli.sh test_contract test_fs test_posix.sh
check_PROGRAMS = test_contract test_fs

test_contract_SOURCES = test_contract.c
test_contract_LDADD = $(top_builddir)/src/libinfcore.a
test_contract_CPPFLAGS = -I$(top_srcdir)/include

test_fs_SOURCES = test_fs.c
test_fs_LDADD = $(top_builddir)/src/libinfcore.a
test_fs_CPPFLAGS = -I$(top_srcdir)/include

EXTRA_DIST = test_cli.sh test_posix.sh
