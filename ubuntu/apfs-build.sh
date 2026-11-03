#!/bin/bash
# Report whether the build tree's apfs_vfsops.c carries the mount-time refresh, and rebuild the
# two objects from the tree as it is.
set -e
S=/home/mrmoxing/k40/xnu-src/xnu-8796.121.2
F=$S/bsd/miscfs/apfs/apfs_vfsops.c
echo "calls in tree: $(grep -c 'apfs_statfs_refresh' "$F")"
grep -n "apfs_statfs_refresh();" "$F" | head -3
O=$S/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT
rm -f "$O/apfs_vfsops.o" "$O/redwood_ufs.o"
cd /home/mrmoxing/k40
DEVICE=alioth ./xnu-build.sh -j16 -k > out/kernel-apfs.log 2>&1 || true
K=$S/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
echo "kernel: $(stat -c '%s B %y' "$K" | cut -c1-40)"
echo -n "marker: "; grep -a -c "root volume answers" "$K" || true
