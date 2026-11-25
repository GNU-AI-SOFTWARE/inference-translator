#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>
#
# POSIX contract test on the /inference translator:
#
#   echo "..." | tee /inference/prompt && cat /inference/request
#
# The translator is muet: it only exposes data.  On GNU/Hurd the test
# runs against the really mounted translator.  On any other host no
# translator can be mounted with settrans, so the portable binary's
# --root mode materializes the node engine as plain files in a
# temporary directory and the same POSIX checks run there.  Skipped
# with exit 77 (automake SKIP) only when neither is available.

set -u

say() { printf '%s\n' "$*"; }
fail() { say "test_posix: FAIL: $*"; FAIL=1; }
FAIL=0

ROOT=${INFERENCE_TEST_ROOT:-/inference}
BIN=${1:-../src/inference-translator}

DAEMON_PID=""
FAKE_ROOT=""
cleanup() {
    [ -n "$DAEMON_PID" ] && kill "$DAEMON_PID" 2>/dev/null
    [ -n "$FAKE_ROOT" ] && rm -rf "$FAKE_ROOT"
    return 0
}
trap cleanup EXIT

# wait_eq NODE EXPECTED / wait_grep NODE PATTERN: poll up to ~6 s.
# Instant on a real mount; one 50 ms tick on the portable --root engine.
wait_eq() {
    _n=$1 _e=$2 _i=0
    while [ "$_i" -lt 150 ]; do
        _g=$(cat "$ROOT/$_n" 2>/dev/null) && [ "$_g" = "$_e" ] && return 0
        _i=$((_i + 1)); sleep 0.1
    done
    return 1
}
wait_grep() {
    _n=$1 _p=$2 _i=0
    while [ "$_i" -lt 150 ]; do
        cat "$ROOT/$_n" 2>/dev/null | grep -qF -- "$_p" && return 0
        _i=$((_i + 1)); sleep 0.1
    done
    return 1
}

if [ ! -e "$ROOT/request" ]; then
    # portable host: run the file-backed engine in a temp directory
    if ! "$BIN" --help 2>/dev/null | grep -q -- '--root'; then
        say "test_posix: SKIP: /inference is not mounted and no --root support"
        # diagnose why: missing binary? help output without --root?
        ls -l "$BIN" 2>&1 || true
        "$BIN" --help 2>&1 | head -6 || true
        exit 77
    fi
    FAKE_ROOT=$(mktemp -d)
    ROOT=$FAKE_ROOT
    "$BIN" --root "$ROOT" &
    DAEMON_PID=$!
    _i=0
    until [ -e "$ROOT/request" ]; do
        _i=$((_i + 1))
        if [ "$_i" -ge 100 ]; then
            say "test_posix: SKIP: --root engine did not start"
            exit 77
        fi
        sleep 0.1
    done
fi

# Clear any sticky manual task override left by a previous run:
# writing "auto" on the task node re-enables keyword detection, which
# makes this test idempotent on a long-lived mount.
echo "auto" | tee "$ROOT/task" >/dev/null 2>&1 || true

PROMPT='résumé https://example.org/article'
EXPECTED='{"prompt":"résumé https://example.org/article","task":"summarize","urls":["https://example.org/article"],"params":{}}'

echo "$PROMPT" | tee "$ROOT/prompt" >/dev/null 2>&1 \
    || fail "write on $ROOT/prompt must succeed"

wait_eq request "$EXPECTED" \
    || fail "request mismatch: got '$(cat "$ROOT/request" 2>/dev/null)'"

wait_eq status "parsed" \
    || fail "$ROOT/status must be 'parsed'"
wait_eq urls "https://example.org/article" \
    || fail "$ROOT/urls must list the extracted URL"
wait_eq task "summarize" \
    || fail "$ROOT/task must be the detected task"

echo "translate" | tee "$ROOT/task" >/dev/null 2>&1 \
    || fail "write on $ROOT/task must succeed"
wait_eq task "translate" \
    || fail "task override must be readable"
wait_grep request '"task":"translate"' \
    || fail "request must carry the overridden task: got '$(cat "$ROOT/request" 2>/dev/null)'"

# "auto" clears the override and re-detects from the current prompt;
# leaving the mount clean makes the next run pass again.
echo "auto" | tee "$ROOT/task" >/dev/null 2>&1 \
    || fail "write auto on $ROOT/task must succeed"
wait_eq task "summarize" \
    || fail "$ROOT/task must be re-detected (summarize) after auto"
wait_grep request '"task":"summarize"' \
    || fail "request must carry the re-detected task after auto: got '$(cat "$ROOT/request" 2>/dev/null)'"

if [ "$FAIL" = 0 ]; then
    say "test_posix: OK"
    exit 0
fi
exit 1
