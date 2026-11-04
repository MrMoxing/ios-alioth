#!/bin/bash
# Sync the file from the repo tree into the WSL build tree, rebuild, verify the new probe.
set -e
cp /mnt/e/Productivity/WorkBuudy/ios-alioth/src/hackios-xnu-kernel-main/xnu/files/bsd/dev/alioth_pcie.c \
   /home/mrmoxing/k40/xnu-src/xnu-8796.121.2/bsd/dev/alioth_pcie.c
cd /home/mrmoxing/k40
DEVICE=alioth ./xnu-build.sh > /tmp/b3.log 2>&1 || true
M=~/k40/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
printf 'probe => '
strings "$M" | grep -c 'the announce goes out before anything of the step is touched' || true
strings "$M" | grep -m1 -oE '[A-Z][a-z]{2} +[A-Z][a-z]{2} +[0-9]+ [0-9:]+ CST 2026'
md5sum "$M"
