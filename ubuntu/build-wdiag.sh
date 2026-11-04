#!/bin/bash
# Kernel with the write-window diagnostics, and a payload whose run is short enough that the
# save-on-return happens long before the log is fetched.
set -e
R=/mnt/e/Productivity/WorkBuudy/ios-alioth/src
cd /home/mrmoxing/k40
cp -f "$R/hackios-xnu-kernel-main/xnu/files/bsd/dev/redwood_ufs.c" xnu/files/bsd/dev/redwood_ufs.c
cp -f "$R/hackios-xnu-kernel-main/xnu/files/bsd/dev/redwood_ufs.c" xnu-src/xnu-8796.121.2/bsd/dev/redwood_ufs.c
rm -f xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/redwood_ufs.o
touch xnu-src/xnu-8796.121.2/bsd/dev/redwood_ufs.c
DEVICE=alioth ./xnu-build.sh -j16 -k > out/kernel-wdiag.log 2>&1 || true
K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
echo "kernel: $(stat -c '%s B' "$K") errors: $(grep -c 'error:' out/kernel-wdiag.log)"
EPOCH=$(date +%s)
CMD="loader_return_after=60 epoch=$EPOCH sb_delay=8 touchscreen=1 buttons=1 touch_stat=5 cpufreq=1 bootcpu=7 cpus=8 adreno_soc=a650 adreno=21 adreno_skip=0 adreno_pause=0 adreno_keep=1 adreno_watch=10 ufs=2 ufs_write=1 var_store=1"
python3 xnuloader/mkpayload.py -o out/run-payload-wdiag.bin kernel=$K ramdisk=userland/root.apfs --cmdline "$CMD" | tail -1
