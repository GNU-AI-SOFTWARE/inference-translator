# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

AC_INIT([inference-translator], [0.1.0], [claire@gnu-ai.org])
AC_CONFIG_SRCDIR([src/main.c])
AM_INIT_AUTOMAKE([-Wall -Werror foreign subdir-objects])

AC_CANONICAL_HOST

# System extensions (POSIX/XSI): defines _GNU_SOURCE etc. in config.h.
# Required by the POSIX.1-2008 interfaces used from phase 1.  This
# macro must run before anything that compiles.
AC_USE_SYSTEM_EXTENSIONS

AC_PROG_CC
AM_PROG_AR
AC_PROG_RANLIB

# Le projet est écrit en C23 ; certains compilateurs nomment encore ce
# mode « c2x ».  Exiger l'un des deux (même politique que
# httpfs-translator, pour une pile homogène).
AC_CACHE_CHECK([whether the C compiler accepts C23], [inf_cv_c23], [
  inf_cv_c23=no
  for inf_flag in -std=c23 -std=c2x; do
    inf_save_CFLAGS=$CFLAGS
    CFLAGS="$CFLAGS $inf_flag"
    AC_COMPILE_IFELSE(
      [AC_LANG_PROGRAM([[
        constexpr int inf_c23_test = 1;
        static void *inf_c23_p = nullptr;]],
        [[return inf_c23_p == nullptr ? inf_c23_test : 0;]])],
      [inf_cv_c23=$inf_flag])
    CFLAGS=$inf_save_CFLAGS
    test "x$inf_cv_c23" != xno && break
  done])
AS_IF([test "x$inf_cv_c23" = xno],
      [AC_MSG_ERROR([a C23 compiler is required (GCC 13 or newer)])])
AC_SUBST([C23_CFLAGS], [$inf_cv_c23])

AC_CONFIG_HEADERS([config.h])

# The --cli interface prototype (src/cli.c) uses ncurses, strictly
# optionally: the PLAN.md rule "buildable with the GNU toolchain
# alone" applies to the base binary, not to the prototype.  Without
# ncurses, HAVE_NCURSES stays undefined and --cli answers with an
# explicit message (tests/test_cli.sh).
AC_CHECK_HEADERS([ncurses.h],
  [inf_cv_ncurses_h=yes], [inf_cv_ncurses_h=no])
AS_IF([test "x$inf_cv_ncurses_h" = xyes],
  [AC_SEARCH_LIBS([initscr], [ncursesw ncurses],
     [inf_cv_ncurses=yes], [inf_cv_ncurses=no])],
  [inf_cv_ncurses=no])
AS_IF([test "x$inf_cv_ncurses" = xyes],
  [AC_DEFINE([HAVE_NCURSES], [1],
     [Define to 1 when ncurses is available for the optional --cli prototype.])],
  [AC_MSG_NOTICE([ncurses not found: --cli prototype disabled, the core still builds])])
AM_CONDITIONAL([HAVE_NCURSES], [test "x$inf_cv_ncurses" = xyes])

# --- GNU/Hurd or not ---------------------------------------------------
# The portable core (contract.c, fs.c, tests) is pure C23/POSIX and
# builds everywhere; only the netfs server loop and the Hurd
# libraries are Hurd-specific.  The host decides which face is
# linked (same policy as data-base-translator):
#   - gnu*/mach* : main-hurd.c + netfs-hooks.c (the /inference
#                  translator), linked with -lnetfs and friends;
#   - otherwise  : main.c (portable verification binary), so `make
#                  check` stays green on any POSIX system.
case "$host_os" in
  gnu*|mach*) hurd_system=yes ;;
  *)          hurd_system=no ;;
esac
AM_CONDITIONAL([ON_HURD], [test "x$hurd_system" = xyes])

# Phase 0 : le squelette n'a aucune dépendance externe — ni trivfs,
# ni netfs, ni éditeur.  Les bibliothèques Hurd n'arrivent qu'avec
# la phase 1 (translator /inference) ; le mode distant de la
# phase 5 n'exigera que le binaire ssh d'OpenSSH, jamais une
# bibliothèque liée.

AC_CONFIG_FILES([
    Makefile
    src/Makefile
    tests/Makefile
])
AC_OUTPUT
