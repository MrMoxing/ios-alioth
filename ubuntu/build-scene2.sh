#!/bin/bash
# Rebuild userland with a chosen scene and pack a payload whose run ends at 100 s, so the log of a
# diagnostic run is complete before the phone returns.
#   build-scene2.sh <scene> <tag>
set -e
cd /home/mrmoxing/k40
SCENE=${1:?scene}
TAG=${2:?tag}
SRC=/mnt/e/Productivity/WorkBuudy/ios-alioth/src/hackios-xnu-userland-main/userland
cp "$SRC/$SCENE" userland/"$SCENE"
cp "$SRC/src/var_store.c" userland/src/var_store.c
# every program and library of ours, not just the store: a diagnostic that lives in an injected
# library (src/inject_sb.c) is no use if the build tree keeps yesterday's copy
cp -r "$SRC/src/." userland/src/
cp "$SRC/build.sh" userland/build.sh
sed -i 's/\r$//' userland/build.sh          # the repository's copy has CRLF; bash in WSL does not
rm -rf userland/var && cp -r "$SRC/var" userland/var
SKIP="kernel loader boot" GL=1 RC_JOBS="$SCENE" bash userland/build.sh alioth > out/scene-$TAG.log 2>&1
echo "userland built: $(stat -c %s userland/root.apfs) B  errors: $(grep -c 'error:' out/scene-$TAG.log)"
K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
EPOCH=$(date +%s)
BASE="loader_return_after=100 epoch=$EPOCH sb_delay=8 touchscreen=1 buttons=1 touch_stat=5 cpufreq=1 bootcpu=7 cpus=8 adreno_soc=a650 adreno=21 adreno_skip=0 adreno_pause=0 adreno_keep=1 adreno_watch=10"
python3 xnuloader/mkpayload.py -o "out/run-payload-$TAG.bin" kernel=$K ramdisk=userland/root.apfs --cmdline "$BASE ufs=2 ufs_write=1 var_store=1 $EXTRA_ARGS" | tail -1
