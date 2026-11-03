#!/bin/bash
# Rebuild the two storage objects from scratch and link. Literal paths only: variables do not
# survive the Windows -> WSL hop.
set -e
OBJ=/home/mrmoxing/k40/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT
SRC=/home/mrmoxing/k40/xnu-src/xnu-8796.121.2
rm -f "$OBJ/redwood_ufs.o" "$OBJ/apfs_vfsops.o"
touch "$SRC/bsd/dev/redwood_ufs.c" "$SRC/bsd/miscfs/apfs/apfs_vfsops.c"
cd /home/mrmoxing/k40
DEVICE=alioth ./xnu-build.sh -j16 -k > out/kernel-apfs.log 2>&1 || true
ls -la "$OBJ/redwood_ufs.o" "$OBJ/apfs_vfsops.o" 2>&1 | tail -2
K=$SRC/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
echo "kernel: $(stat -c '%s B %y' "$K" | cut -c1-40)"
echo -n "diagnostic: "; grep -a -c "ufs_shared_pa parsed" "$K" || true
grep -nE "error:|Undefined symbols" out/kernel-apfs.log | head -4 || true
