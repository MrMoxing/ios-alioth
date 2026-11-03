#!/bin/bash
# Rebuild the shipped pair: root.apfs from the default scene, and a payload whose cmdline asks
# the loader for the block service (ufs=2) and to keep /private/var in the flash between boots
# (ufs_write=1 var_store=1, into logdump's tail: var_store.c says where and why).
set -e
cd /home/mrmoxing/k40
cp /mnt/e/Productivity/WorkBuudy/ios-alioth/src/hackios-xnu-userland-main/userland/src/var_store.c userland/src/var_store.c
SKIP="kernel loader boot" GL=1 bash userland/build.sh alioth > out/release-root.log 2>&1
echo "userland rc=$?  root.apfs $(stat -c %s userland/root.apfs) B"
head -2 userland/root/etc/rc.jobs | sed 's/^/  scene: /'
K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
EPOCH=$(date +%s)
CMD="loader_return_after=240 epoch=$EPOCH sb_delay=8 touchscreen=1 buttons=1 touch_stat=5 cpufreq=1 bootcpu=7 cpus=8 adreno_soc=a650 adreno=21 adreno_skip=0 adreno_pause=0 adreno_keep=1 adreno_watch=10 ufs=2 ufs_write=1 var_store=1"
python3 xnuloader/mkpayload.py -o out/run-payload-release.bin kernel=$K ramdisk=userland/root.apfs --cmdline "$CMD" | tail -1
ls -la out/run-payload-release.bin
