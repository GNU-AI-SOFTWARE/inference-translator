#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>
#
# POSIX contract test on the really mounted translator (GNU/Hurd):
#
#   echo "..." | tee /inference/prompt && cat /inference/request
#
# The translator is muet: it only exposes data.  Skipped with exit 77
# (automake SKIP) when /inference is not mounted -- e.g. in a
# non-Hurd build sandbox.

set -u

say() { printf '%s\n' "$*"; }
fail() { say "test_posix: FAIL: $*"; FAIL=1; }
FAIL=0

if [ ! -e /inference/request ]; then
    say "test_posix: SKIP: /inference is not mounted"
    exit 77
fi

PROMPT='résume https://example.org/article'
EXPECTED='{"prompt":"résume https://example.org/article","task":"summarize","urls":["https://example.org/article"],"params":{}}'

echo "$PROMPT" | tee /inference/prompt >/dev/null 2>&1 \
    || fail "write on /inference/prompt must succeed"

GOT=$(cat /inference/request 2>/dev/null) \
    || fail "read of /inference/request must succeed"
[ "$GOT" = "$EXPECTED" ] || fail "request mismatch: got '$GOT'"

[ "$(cat /inference/status 2>/dev/null)" = "parsed" ] \
    || fail "/inference/status must be 'parsed'"
[ "$(cat /inference/urls 2>/dev/null)" = "https://example.org/article" ] \
    || fail "/inference/urls must list the extracted URL"
[ "$(cat /inference/task 2>/dev/null)" = "summarize" ] \
    || fail "/inference/task must be the detected task"

echo "translate" | tee /inference/task >/dev/null 2>&1 \
    || fail "write on /inference/task must succeed"
[ "$(cat /inference/task 2>/dev/null)" = "translate" ] \
    || fail "task override must be readable"
GOT=$(cat /inference/request 2>/dev/null)
case "$GOT" in
    *'"task":"translate"'*) : ;;
    *) fail "request must carry the overridden task: got '$GOT'" ;;
esac

if [ "$FAIL" = 0 ]; then
    say "test_posix: OK"
    exit 0
fi
exit 1
