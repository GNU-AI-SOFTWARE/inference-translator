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
# The build is offline by default (pre-injected tarball below), but
# the guest network is set up at the top of this script (guest_net:
# pfinet on eth0 with the QEMU user-mode parameters, resolv.conf,
# DNS retries, /etc/hosts fallbacks) so the git-clone fallback and
# any HF smoke test work.  The preinstalled image ships gcc and make
# but no autoconf/automake/git, so pre-generate configure on the
# host, pack the tree and inject it into the CI image first:
#
#   git clone https://github.com/GNU-AI-SOFTWARE/inference-translator ~/inference-translator
#   cd ~/inference-translator && autoreconf -i && cd ~
#   tar czf inference-src.tar.gz inference-translator
#   ./inject-src.sh <image> ~/inference-src.tar.gz
#
# autoreconf -i must SUCCEED on the host (automake >= 1.17 hard-errors on
# a top-level install.sh, now moved to scripts/); below, the guest checks
# that the generated build system is complete and normalizes timestamps,
# so make can never try to run the absent automake through ./missing.
#
# If /root/inference-src.tar.gz is missing but git and the network
# work, falls back to a git clone.

set -eu
export DEBIAN_FRONTEND=noninteractive
# locale-clean builds: the ssh client often sends LANG/LC_* for a locale
# the guest image has not generated yet, which breaks perl/groupadd/dpkg
# mid-apt ("invalid group ID ''" -> ssl-cert/postgresql left unconfigured)
export LANG=C.UTF-8 LC_ALL=C.UTF-8

# --- guest network ------------------------------------------------------
# QEMU user-mode networking (10.0.2.0/24, gateway 10.0.2.2, DNS
# 10.0.2.3).  The image's NIC is driven by netdde, started by ifup at
# boot with DHCP from slirp (bound to 10.0.2.15); a stale
# /run/network/ifstate can make ifup skip eth0, so clear it and
# re-run ifup.  NEVER attach pfinet with -fg on /servers/socket/2:
# that kills netdde and removes /dev/eth0, leaving the guest with no
# network at all (the QEMU hostfwd then answers "Connection reset").
# pfinet is a last resort, only on a free socket server.  resolv.conf,
# DNS-over-TCP retry and /etc/hosts fallbacks as before.  Best effort:
# markers only, the build itself is offline.
guest_net () {
    # Addresses come from the driver (HURD_VM_NET=...), defaults 10.0.2.x.
    GUEST_ADDR=${HURD_VM_GUEST_ADDR:-10.0.2.15}
    GUEST_MASK=${HURD_VM_GUEST_MASK:-255.255.255.0}
    GUEST_GW=${HURD_VM_GUEST_GW:-10.0.2.2}
    GUEST_DNS=${HURD_VM_GUEST_DNS:-10.0.2.3}
    rm -f /etc/resolv.conf
    printf 'nameserver %s\noptions timeout:2 attempts:2\n' "$GUEST_DNS" \
        > /etc/resolv.conf
    # NOTE: this script runs under set -e; every fallible network
    # command below must be guarded with ||, or the whole guest script
    # aborts with rc=1 before any build output.
    if [ "${HURD_VM_MODE:-slirp}" = "tap" ]; then
        # bridged mode (HURD_VM_TAP on the host): no slirp DHCP, pin the
        # LAN address directly
        echo "tap mode: static $GUEST_ADDR gw $GUEST_GW dns $GUEST_DNS"
        ifconfig eth0 "$GUEST_ADDR" netmask "$GUEST_MASK" up 2>&1 || true
        route add default "$GUEST_GW" 2>&1 || true
        sleep 1
    elif ifconfig eth0 2>/dev/null | grep -q "inet $GUEST_ADDR"; then
        echo "eth0 already $GUEST_ADDR (boot DHCP via netdde)"
    else
        rm -rf /run/network/ifstate 2>/dev/null || true
        mkdir -p /run/network 2>/dev/null || true
        : > /run/network/ifstate 2>/dev/null || true
        ifdown eth0 2>/dev/null || true
        rc=0
        ifup eth0 2>&1 || rc=$?
        echo "ifup eth0 rc=$rc"
        sleep 2
        if ! ifconfig eth0 2>/dev/null | grep -q "inet $GUEST_ADDR"; then
            # ifup failed (stale ifstate at boot, or a translator already
            # on /servers/socket/2): configure the deterministic slirp
            # address statically.  On Hurd, ifconfig also starts the
            # passive translator on /servers/socket/2.
            echo "WARN: ifup rc=$rc without $GUEST_ADDR; static ifconfig fallback"
            ifconfig eth0 "$GUEST_ADDR" netmask "$GUEST_MASK" up 2>&1 || true
            route add default "$GUEST_GW" 2>&1 || true
            sleep 1
        fi
    fi
    if ! ifconfig eth0 2>/dev/null | grep -q "inet $GUEST_ADDR"; then
        echo "WARN: eth0 has no $GUEST_ADDR; device and server state:"
        ifconfig -a 2>&1 || true
        ls -l /dev/eth* 2>&1 || true
        showtrans /servers/socket/2 2>&1 || true
        if [ -e /dev/eth0 ]; then
            echo "WARN: /dev/eth0 exists; dhclient -1 fallback"
            timeout 20 dhclient -1 /dev/eth0 2>&1 | tail -5 || true
            sleep 1
        fi
        if ! showtrans /servers/socket/2 2>&1 | grep -q .; then
            echo "WARN: /servers/socket/2 free; last resort: static pfinet"
            settrans -a /servers/socket/2 /hurd/pfinet \
                -i /dev/eth0 -a "$GUEST_ADDR" -m "$GUEST_MASK" -g "$GUEST_GW" 2>&1 \
                || echo "WARN: pfinet attach rc=$?"
            sleep 2
            ifconfig -a 2>&1 || true
        else
            echo "WARN: /servers/socket/2 already has a translator; not touching it"
        fi
    fi
    i=0
    until getent hosts deb.debian.org >/dev/null 2>&1; do
        i=$((i+1))
        [ "$i" -ge 10 ] && break
        sleep 2
    done
    if ! getent hosts deb.debian.org >/dev/null 2>&1; then
        printf 'nameserver %s\noptions usevc timeout:2 attempts:2\n' "$GUEST_DNS" \
            > /etc/resolv.conf
    fi
    pin_host () {
        ip=$1 name=$2
        getent hosts "$name" >/dev/null 2>&1 && return 0
        grep -qw "$name" /etc/hosts 2>/dev/null && return 0
        echo "$ip $name" >> /etc/hosts
    }
    pin_host 18.239.50.80 huggingface.co
    pin_host 18.239.105.46 datasets-server.huggingface.co
    pin_host 140.82.121.4 github.com
    # slirp's UDP DNS (10.0.2.3) is often broken on Hurd; apt/git need
    # deb.debian.org, so pin it too (resolved on the host beforehand):
    pin_host 194.71.11.165 deb.debian.org
    pin_host 194.71.11.165 cdimage.debian.org
    if getent hosts deb.debian.org >/dev/null 2>&1; then
        echo "DNS-OK"
    else
        echo "DNS-KO (offline mode)"
    fi
    if wget -q -O /dev/null --timeout=5 http://deb.debian.org/ 2>/dev/null; then
        echo "HTTP-OK"
    else
        echo "HTTP-KO (offline mode)"
    fi
}
guest_net

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

# The guest image has no autoconf/automake: require a complete
# pre-generated build system (clear GUEST-BUILD-KO marker otherwise),
# then normalize timestamps so make never regenerates configure or
# Makefile.in — that would call the absent automake via ./missing and
# die with "make: *** [Makefile:...: Makefile.in] Error 1".
if [ ! -x ./configure ]; then
    if command -v autoreconf >/dev/null 2>&1; then
        autoreconf -i
    else
        echo "GUEST-BUILD-KO: no ./configure and no autoreconf in the guest" >&2
        echo "host side: cd ~/inference-translator && autoreconf -i, re-pack, re-inject" >&2
        exit 1
    fi
fi
for f in Makefile.in src/Makefile.in tests/Makefile.in config.h.in; do
    [ -f "$f" ] || {
        echo "GUEST-BUILD-KO: $f missing — run autoreconf -i on the host before packing" >&2
        exit 1
    }
done
touch aclocal.m4 configure config.h.in Makefile.in src/Makefile.in tests/Makefile.in

./configure && make

# Mount the translator BEFORE make check: on Hurd, test_posix.sh then
# runs for real instead of skipping (exit 77); on other systems
# /inference stays unmounted and test_posix.sh skips as designed.
settrans -g /inference 2>/dev/null || true   # drop any stale mount
mkdir -p /inference
settrans -a /inference ./src/inference-translator

make check || { cat tests/test-suite.log 2>/dev/null || true; exit 1; }
# expected on Hurd (6 tests): test_cli.sh, test_client.sh (ptyrun
# fallback: no script(1) on Hurd), test_contract 36/36, test_fs 34/34,
# test_client 120/120, test_posix.sh OK (mounted).  test_posix.sh ends
# with the task override reset to "auto" (re-detected), so the manual
# demo below starts clean and sets "translate" explicitly.

# Manual demo of the POSIX interface (make check already ran
# test_posix.sh against the mounted translator):
echo "résume https://example.org/article" | tee /inference/prompt
echo "--- request ---"; cat /inference/request; echo
echo "--- status ---"; cat /inference/status; echo
echo "--- urls ---";   cat /inference/urls; echo

echo translate | tee /inference/task
echo "--- request (override) ---"; cat /inference/request; echo   # must contain "task":"translate"

./src/inference-translator --version    # 0.1.0

# phase 2 client: smoke test against the mounted translator
./src/inference --version               # inference (GNU AI) 0.1.0
if ./src/inference --mount /inference </dev/null >/dev/null 2>&1; then
    echo "FAIL: the client must refuse a non-TTY stdin" >&2
    exit 1
fi

settrans -g /inference

# network smoke marker (informational; the build above is offline)
if getent hosts huggingface.co >/dev/null 2>&1; then
    echo "HF-DNS-OK"
else
    echo "HF-DNS-KO (hosts fallback pinned)"
fi

echo GUEST-INF-OK
