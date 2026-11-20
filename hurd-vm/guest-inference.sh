#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>
#
# Build and test GNU-AI-SOFTWARE/inference-translator on real
# GNU/Hurd, then mount the /inference translator and exercise it
# through plain POSIX commands.
#
#   python3 hurd_vm.py <image> guest-inference.sh --ram 2G
#
# Offline by default: the preinstalled image ships gcc and make but
# no autoconf/automake/git, and its network is unreliable (see
# guest-netfix.sh).  Pre-generate configure on the host, pack the
# tree and inject it into the CI image first:
#
#   git clone https://github.com/GNU-AI-SOFTWARE/inference-translator ~/inference-translator
#   cd ~/inference-translator && autoreconf -i && cd ~
#   tar czf inference-src.tar.gz inference-translator
#   ./inject-src.sh <image> ~/inference-src.tar.gz
#
# If /root/inference-src.tar.gz is missing but git and the network
# work, falls back to a git clone.

set -eu
export DEBIAN_FRONTEND=noninteractive

cd /root
if [ -f inference-src.tar.gz ]; then
    rm -rf inference-translator
    tar xf inference-src.tar.gz
elif command -v git >/dev/null 2>&1 \
     && getent hosts github.com >/dev/null 2>&1; then
    rm -rf inference-translator
    git clone https://github.com/GNU-AI-SOFTWARE/inference-translator
else
    echo "no /root/inference-src.tar.gz and no usable network:" >&2
    echo "run inject-src.sh on the host first (see header comment)" >&2
    exit 1
fi
cd inference-translator

./configure && make && make check
# expected: test_contract 36/36, test_fs 28/28, test_cli.sh OK,
#           test_posix.sh SKIP 77 (/inference not mounted yet)

mkdir -p /inference
settrans -a /inference ./src/inference-translator

echo "résume https://example.org/article" | tee /inference/prompt
echo "--- request ---"; cat /inference/request
# expected: {"prompt":"résume https://example.org/article","task":"summarize","urls":["https://example.org/article"],"params":{}}
echo "--- status ---"; cat /inference/status     # expected: parsed
echo "--- urls ---";   cat /inference/urls       # expected: https://example.org/article
echo "--- task ---";   cat /inference/task       # expected: summarize

echo translate | tee /inference/task
echo "--- request (override) ---"; cat /inference/request   # must contain "task":"translate"

sh tests/test_posix.sh                  # mounted now: must pass (no SKIP)
./src/inference-translator --version    # 0.1.0

settrans -g /inference
echo GUEST-INF-OK
