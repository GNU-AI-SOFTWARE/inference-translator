#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>
#
# Inject a file into /root/ of the prepared CI image without booting
# the VM: extract the root partition with dd, write the file with
# debugfs, put the partition back.  Used to feed pre-built source
# tarballs to guest scripts when the guest network is unusable.
#
#   ./inject-src.sh <file-to-inject>
#   ./inject-src.sh <source-image> <file-to-inject>
#   e.g. ./inject-src.sh ~/inference-src.tar.gz
#        ./inject-src.sh debian-hurd-amd64-20260314.img ~/inference-src.tar.gz
#
# If the prepared CI copy (debian-hurd-ci.img) does not exist yet and a
# source image is given, it is prepared first via hurd_vm.prepare_ci_image.
#
# Requires: no VM running on the image, e2fsprogs (e2fsck/debugfs are
# looked up in /usr/sbin and /sbin too, via hurd_vm.find_tool), python3
# (uses hurd_vm.root_partition).  The CI image is modified in place;
# pass --fresh to hurd_vm.py afterwards to rebuild it from scratch.

set -eu

case $# in
    1) src=;   file=$1 ;;
    2) src=$1; file=$2 ;;
    *) echo "usage: inject-src.sh [source-image] <file>" >&2; exit 2 ;;
esac
here=$(cd "$(dirname "$0")" && pwd)
# Same default as hurd_vm.py (CI_IMAGE), overridable the same way;
# the CI copy is always named debian-hurd-ci.img next to hurd_vm.py:
ci=${HURD_VM_CI_IMAGE:-$here/debian-hurd-ci.img}
if [ ! -f "$ci" ]; then
    # No prepared CI copy yet (e.g. fresh CI runner): build it from the
    # source image, exactly like hurd_vm.py's first run would.
    [ -n "$src" ] || { echo "no CI image: $ci and no source image given" >&2; exit 1; }
    [ -f "$src" ] || { echo "no such source image: $src" >&2; exit 1; }
    echo "preparing the CI copy from $src (one-time)..."
    (cd "$here" && python3 -c "import hurd_vm as h; h.prepare_ci_image('$src')")
fi
[ -f "$file" ] || { echo "no such file: $file" >&2; exit 1; }

read -r lba sectors e2fsck debugfs <<PYEOF
$(cd "$here" && python3 -c "
import hurd_vm as h
print(*h.root_partition('$ci'), h.find_tool('e2fsck'), h.find_tool('debugfs'))
")
PYEOF

part=$(mktemp /tmp/hurd-rootpart.XXXXXX)
trap 'rm -f "$part"' EXIT
dd if="$ci" of="$part" bs=512 skip="$lba" count="$sectors" status=none
# e2fsck exit codes: 0 = clean, 1-3 = errors corrected, >=4 = unfixable.
# Under set -e a corrected filesystem (rc=1) must NOT abort the script:
set +e
"$e2fsck" -fy "$part" >/dev/null 2>&1
frc=$?
set -e
[ "$frc" -lt 4 ] || { echo "e2fsck failed (rc=$frc) on the extracted partition" >&2; exit 1; }
# rm first: debugfs "write" refuses to overwrite an existing file
"$debugfs" -w -R "rm /root/$(basename "$file")" "$part" >/dev/null 2>&1 || true
"$debugfs" -w -R "write $(readlink -f "$file") /root/$(basename "$file")" "$part"
dd if="$part" of="$ci" bs=512 seek="$lba" conv=notrunc status=none
echo "injected $file into $ci as /root/$(basename "$file")"
