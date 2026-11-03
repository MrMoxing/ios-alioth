#!/bin/bash
# Rebuild root.apfs with a chosen scenario and pack a payload with a chosen cmdline.
#   build-scene.sh <scene> <payload-tag> [extra cmdline...]
set -e
cd /home/mrmoxing/k40
SCENE=${1:?scene}
TAG=${2:?tag}
shift 2
EXTRA="$*"
cp "/mnt/e/Productivity/WorkBuudy/ios-alioth/src/hackios-xnu-userland-main/userland/$SCENE" userland/"$SCENE"
SKIP="kernel loader boot" GL=1 RC_JOBS="$SCENE" bash userland/build.sh alioth > out/scene-$SCENE.log 2>&1
echo "userland RC=$?"
ls -la userland/root.apfs
head -3 userland/root/etc/rc.jobs | sed 's/^/  rc.jobs: /'
K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
EPOCH=$(date +%s)
BASE="loader_return_after=200 epoch=$EPOCH sb_delay=8 touchscreen=1 buttons=1 touch_stat=5 cpufreq=1 bootcpu=7 cpus=8 adreno_soc=a650 adreno=21 adreno_skip=0 adreno_pause=0 adreno_keep=1 adreno_watch=10"
python3 xnuloader/mkpayload.py -o "out/run-payload-$TAG.bin" kernel=$K ramdisk=userland/root.apfs --cmdline "$BASE $EXTRA" | tail -1
ls -la "out/run-payload-$TAG.bin"
