#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>
#
# End-to-end test of the phase 2 `inference` client against a fake
# mount point (a plain directory), driven through a pseudo-terminal
# so the client believes it has a TTY:
#
#   - non-TTY stdin must be refused with an explicit message (exit 2)
#   - banner + narration naming the POSIX nodes
#   - /submit writes the composed buffer to <mount>/prompt
#   - /task validates the task name
#   - /editor with a fake $VISUAL: exit 0 -> buffer re-read and
#     submitted; exit != 0 -> buffer unchanged
#
# The pty driver is `script(1)` when available (Linux), otherwise the
# bundled ptyrun.c compiled on the fly: util-linux does not build
# script(1) on GNU/Hurd (Debian #807071).  Set
# INFERENCE_TEST_FORCE_PTYRUN=1 to exercise the ptyrun path on Linux
# too.  Skipped with exit 77 only when neither script(1) nor a C
# compiler is available.

set -u

say() { printf '%s\n' "$*"; }
fail() { say "test_client: FAIL: $*"; FAIL=1; }
FAIL=0

TESTDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BIN=${1:-../src/inference}
[ -x "$BIN" ] || { say "test_client: SKIP: no built client at $BIN"; exit 77; }

D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT

if command -v script >/dev/null 2>&1 && [ "${INFERENCE_TEST_FORCE_PTYRUN:-0}" != 1 ]; then
    pty() { script -qec "$1" /dev/null; }
else
    PTY="$D/ptyrun"
    ${CC:-cc} -O2 -o "$PTY" "$TESTDIR/ptyrun.c" 2>/dev/null \
        || { say "test_client: SKIP: no script(1) and no C compiler for ptyrun.c"; exit 77; }
    pty() { "$PTY" sh -c "$1"; }
fi

M="$D/inference"
mkdir -p "$M"
printf 'idle' > "$M/status"

# --- non-TTY refusal ------------------------------------------------

"$BIN" --mount "$M" </dev/null >/dev/null 2>"$D/err"
rc=$?
[ "$rc" = 2 ] || fail "non-TTY must exit 2, got $rc"
grep -q 'terminal' "$D/err" || fail "non-TTY refusal must mention the terminal"

# --- pty session: compose, /status, /submit, /quit ------------------

printf 'bonjour https://example.org\n/status\n/submit\n/quit\n' \
    | pty "$BIN --mount $M" >"$D/out" 2>&1
sed 's/\x1b\[[0-9;]*m//g' "$D/out" > "$D/out.clean"

grep -q 'inference (GNU AI)' "$D/out.clean" || fail "banner missing"
grep -q "read $M/status" "$D/out.clean" || fail "narration read status missing"
grep -q "write $M/prompt" "$D/out.clean" || fail "narration write prompt missing"
grep -q "read $M/request" "$D/out.clean" || fail "request narration missing"
[ "$(cat "$M/prompt" 2>/dev/null)" = "bonjour https://example.org" ] \
    || fail "/submit must write the buffer to <mount>/prompt, got '$(cat "$M/prompt" 2>/dev/null)'"

# --- /task validation -------------------------------------------------

printf '/task bogus\n/task summarize\n/quit\n' \
    | pty "$BIN --mount $M" >"$D/out2" 2>&1
sed 's/\x1b\[[0-9;]*m//g' "$D/out2" > "$D/out2.clean"
grep -q 'tâche inconnue' "$D/out2.clean" || fail "/task bogus must be refused"
grep -q "write $M/task" "$D/out2.clean" || fail "/task must narrate the task node"
[ "$(cat "$M/task" 2>/dev/null)" = "summarize" ] \
    || fail "/task summarize must write <mount>/task"

# --- /editor with a fake $VISUAL --------------------------------------

cat > "$D/fake-editor-ok.sh" <<'EOS'
#!/bin/sh
printf 'édité dans $VISUAL' > "$1"
exit 0
EOS
cat > "$D/fake-editor-ko.sh" <<'EOS'
#!/bin/sh
exit 3
EOS
chmod +x "$D/fake-editor-ok.sh" "$D/fake-editor-ko.sh"

export VISUAL="$D/fake-editor-ok.sh"
printf '/editor\n/submit\n/quit\n' \
    | pty "$BIN --mount $M" >"$D/out3" 2>&1
sed 's/\x1b\[[0-9;]*m//g' "$D/out3" > "$D/out3.clean"
grep -q 'buffer relu' "$D/out3.clean" || fail "/editor must re-read the buffer on exit 0"
[ "$(cat "$M/prompt" 2>/dev/null)" = "édité dans \$VISUAL" ] \
    || fail "/editor exit 0: submitted prompt must be the edited buffer"

export VISUAL="$D/fake-editor-ko.sh"
printf 'texte avant\n/editor\n/submit\n/quit\n' \
    | pty "$BIN --mount $M" >"$D/out4" 2>&1
sed 's/\x1b\[[0-9;]*m//g' "$D/out4" > "$D/out4.clean"
unset VISUAL
grep -q 'buffer inchangé' "$D/out4.clean" || fail "/editor exit!=0 must keep the buffer"
[ "$(cat "$M/prompt" 2>/dev/null)" = "texte avant" ] \
    || fail "/editor exit!=0: submitted prompt must stay the previous buffer"

if [ "$FAIL" = 0 ]; then
    say "test_client: OK"
    exit 0
fi
exit 1
