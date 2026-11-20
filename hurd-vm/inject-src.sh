#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>
#
# Inject a file into /root/ of the prepared CI image without booting
# the VM: extract the root partition with dd, write the file with
# debugfs, put the partition back.  Used to feed pre-built source
# tarballs to guest scripts when the guest network is unusable.
#
#   ./inject-src.sh <source-image> <file-to-inject>
#   e.g. ./inject-src.sh debian-hurd-amd64-20260314.img ~/inference-src.tar.gz
#
# Requires: no VM running on the image, e2fsprogs, python3 (uses
# hurd_vm.root_partition).  The CI image is modified in place; pass
# --fresh to hurd_vm.py afterwards to rebuild it from scratch.

set -eu

src=${1:?usage: inject-src.sh <source-image> <file>}
file=${2:?missing file}
here=$(cd "$(dirname "$0")" && pwd)
ci=${HURD_VM_CI_IMAGE:-}
if [ -z "$ci" ]; then
    ci="$here/$(basename "$src" .img)-ci.img"
fi
[ -f "$ci" ] || { echo "no CI image: $ci (run hurd_vm.py once first)" >&2; exit 1; }
[ -f "$file" ] || { echo "no such file: $file" >&2; exit 1; }

read -r lba sectors <<PYEOF
$(cd "$here" && python3 -c "import hurd_vm as h; print(*h.root_partition('$ci'))")
PYEOF

part=$(mktemp /tmp/hurd-rootpart.XXXXXX)
trap 'rm -f "$part"' EXIT
dd if="$ci" of="$part" bs=512 skip="$lba" count="$sectors" status=none
e2fsck -fy "$part" >/dev/null
debugfs -w -R "write $(readlink -f "$file") /root/$(basename "$file")" "$part"
dd if="$part" of="$ci" bs=512 seek="$lba" conv=notrunc status=none
echo "injected $file into $ci as /root/$(basename "$file")"
