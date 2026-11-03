#!/bin/bash
# Rebuild userland with the persistence scene, and a payload whose run ends at 100 s so that the
# save-on-return happens while the phone is still warm and the log can be read back afterwards.
set -e
cd /home/mrmoxing/k40
SCENE=rc.jobs.persist
TAG=${1:-persist2}
cp "/mnt/e/Productivity/WorkBuudy/ios-alioth/src/hackios-xnu-userland-main/userland/$SCENE" userland/"$SCENE"
cp "/mnt/e/Productivity/WorkBuudy/ios-alioth/src/hackios-xnu-userland-main/userland/src/var_store.c" userland/src/var_store.c
rm -f userland/obj/var_store.o 2>/dev/null
grep -n "define SLOT_BYTES\|define STORE_AT\|define DEVICE" userland/src/var_store.c | sed 's/^/  /'
SKIP="kernel loader boot" GL=1 RC_JOBS="$SCENE" bash userland/build.sh alioth > out/scene-$TAG.log 2>&1
echo "userland built: $(stat -c '%s B' userland/root.apfs)"
grep -c "var_store" userland/root/etc/rc.jobs | sed 's/^/  scene lines: /'
K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
EPOCH=$(date +%s)
BASE="loader_return_after=100 epoch=$EPOCH sb_delay=8 touchscreen=1 buttons=1 touch_stat=5 cpufreq=1 bootcpu=7 cpus=8 adreno_soc=a650 adreno=21 adreno_skip=0 adreno_pause=0 adreno_keep=1 adreno_watch=10"
python3 xnuloader/mkpayload.py -o "out/run-payload-$TAG.bin" kernel=$K ramdisk=userland/root.apfs --cmdline "$BASE ufs=2 ufs_write=1 var_store=1" | tail -1
ls -la "out/run-payload-$TAG.bin"
