#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>
#
# install.sh — installation automatique de inference-translator :
# dépendances (si root et apt présent), autogen, configure, make,
# make check, make install.
#
#   ./scripts/install.sh                  # préfixe /usr/local, tests inclus
#   ./scripts/install.sh --prefix=/usr
#   ./scripts/install.sh --no-check       # saute make check
#   ./scripts/install.sh --no-deps        # ne touche pas aux paquets
#   ./scripts/install.sh --help

set -eu

PREFIX=/usr/local
DO_CHECK=yes
DO_DEPS=yes

usage() {
    sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix=*) PREFIX=${1#--prefix=} ;;
        --prefix)   shift; PREFIX=${1:?} ;;
        --no-check) DO_CHECK=no ;;
        --no-deps)  DO_DEPS=no ;;
        -h|--help)  usage; exit 0 ;;
        *) echo "option inconnue: $1 (voir --help)" >&2; exit 2 ;;
    esac
    shift
done

cd "$(dirname "$0")/.."

echo "==> hôte: $(uname -s) $(uname -m)"

# --- 1. dépendances de compilation (root + apt seulement) -------------
if [ "$DO_DEPS" = yes ] && [ "$(id -u)" = 0 ] && command -v apt-get >/dev/null 2>&1; then
    echo "==> installation des dépendances (apt-get)"
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    case "$(uname -s)" in
        GNU) apt-get install -y -qq build-essential gcc make \
                 autoconf automake libtool git hurd-dev gnumach-dev ;;
        *)   apt-get install -y -qq build-essential make \
                 autoconf automake libtool libncurses-dev ;;
    esac
    # hurd-amd64 : hurd-dev installe idvec.h hors de /usr/include/hurd
    if [ -e /usr/include/x86_64-gnu/idvec.h ] && [ ! -e /usr/include/hurd/idvec.h ]; then
        mkdir -p /usr/include/hurd
        ln -sf /usr/include/x86_64-gnu/idvec.h /usr/include/hurd/idvec.h
        echo "==> lien /usr/include/hurd/idvec.h posé"
    fi
else
    echo "==> dépendances non installées (--no-deps ou pas root/apt) :"
    echo "    gcc C23, make, autoconf, automake, libtool requis"
    [ "$(uname -s)" = GNU ] && echo "    + hurd-dev gnumach-dev sur GNU/Hurd"
fi

# --- 2. build ------------------------------------------------------------
echo "==> autogen (autoreconf -i)"
if [ -x ./autogen.sh ]; then ./autogen.sh >/dev/null; else autoreconf -i; fi

echo "==> configure --prefix=$PREFIX"
./configure --prefix="$PREFIX"

echo "==> make"
make

if [ "$DO_CHECK" = yes ]; then
    echo "==> make check"
    make check
fi

# --- 3. installation -----------------------------------------------------
if [ "$(id -u)" = 0 ]; then
    echo "==> make install (prefix=$PREFIX)"
    make install
else
    echo "==> sudo make install (prefix=$PREFIX)"
    sudo make install
fi

echo
echo "Installation terminée. Binaires : $PREFIX/bin/inference-translator, $PREFIX/bin/inference"
echo "Man page : man inference   |   Sur Hurd : settrans -a /inference $PREFIX/bin/inference-translator"
echo "Voir INSTALL.md pour l'utilisation, hurd-vm/README.md pour la VM de test."
