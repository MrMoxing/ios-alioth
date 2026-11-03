#!/bin/bash
K=/home/mrmoxing/k40/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
echo "kernel: $(stat -c '%s bytes %y' "$K" 2>/dev/null | cut -c1-40)"
echo -n "marker in kernel: "
grep -a -c "root volume answers" "$K" 2>/dev/null || echo 0
echo -n "refresh symbol: "
/home/mrmoxing/k40/xnu-deps/host/bin/llvm-nm "$K" 2>/dev/null | grep -c apfs_statfs_refresh
