#!/bin/bash
# Second build pass: show the real error if any, and the kernel version string.
set -e
cd /home/mrmoxing/k40
DEVICE=alioth ./xnu-build.sh > /tmp/build2.log 2>&1 || true
grep -E "error|Error" /tmp/build2.log | grep -v "Wno-error" | head -10 || echo "no errors in log"
grep -m1 "GNU time\|clang version\|version" /tmp/build2.log | head -3
M=~/k40/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
strings "$M" | grep -m2 "19:55:41\|20:22:13\|[0-9][0-9]:[0-9][0-9]:[0-9][0-9]"
strings "$M" | grep -m1 "the %u ms delay is over"
md5sum "$M"
