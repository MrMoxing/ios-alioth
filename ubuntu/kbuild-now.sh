#!/bin/bash
# Rebuild the kernel from the build tree as it stands (no source sync), then repack a payload.
#   kbuild-now.sh <payload-tag> [extra cmdline...]
set -e
cd /home/mrmoxing/k40
TAG=${1:?tag}
shift
EXTRA="$*"
DEVICE=alioth ./xnu-build.sh -j16 -k > out/kernel-now.log 2>&1 || true
K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
echo "errors: $(grep -c 'error:' out/kernel-now.log)"
ls -la "$K" 2>/dev/null | awk '{print $5" B  "$6" "$7" "$8}'
strings "$K" 2>/dev/null | grep -c "statfs refresh" | sed 's/^/statfs refresh strings: /'
EPOCH=$(date +%s)
BASE="loader_return_after=200 epoch=$EPOCH sb_delay=8 touchscreen=1 buttons=1 touch_stat=5 cpufreq=1 bootcpu=7 cpus=8 adreno_soc=a650 adreno=21 adreno_skip=0 adreno_pause=0 adreno_keep=1 adreno_watch=10"
python3 xnuloader/mkpayload.py -o "out/run-payload-$TAG.bin" kernel=$K ramdisk=userland/root.apfs --cmdline "$BASE $EXTRA" | tail -1
ls -la "out/run-payload-$TAG.bin"
