#!/bin/bash
# Persistence build: the loader that serves logdump's tail, the kernel that accepts it, the
# userland whose var_store writes there, and a payload that asks for all three.
set -e
R=/mnt/e/Productivity/WorkBuudy/ios-alioth/src
cd /home/mrmoxing/k40

# --- sources into the build tree -------------------------------------------------------------
cp -f "$R/hackios-xnu-kernel-main/xnuloader/loader.c" xnuloader/loader.c
cp -f "$R/hackios-xnu-kernel-main/xnu/files/bsd/dev/redwood_ufs.c" xnu/files/bsd/dev/redwood_ufs.c
cp -f "$R/hackios-xnu-kernel-main/xnu/files/bsd/dev/redwood_ufs.c" xnu-src/xnu-8796.121.2/bsd/dev/redwood_ufs.c
cp -f "$R/hackios-xnu-userland-main/userland/src/var_store.c" userland/src/var_store.c
echo "sources: in place"

# --- loader and boot image -------------------------------------------------------------------
SKIP="kernel images" ./common/build.sh alioth > out/build-loader.log 2>&1 || echo "loader rc=$?"
ls -la xnuloader/loader.efi out/alioth-boot.img 2>/dev/null | awk '{print "  "$5" B  "$9}'
md5sum out/alioth-boot.img | cut -c1-32

# --- kernel (the write window) ---------------------------------------------------------------
KOBJ=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT
rm -f "$KOBJ/redwood_ufs.o"
touch xnu-src/xnu-8796.121.2/bsd/dev/redwood_ufs.c
DEVICE=alioth ./xnu-build.sh -j16 -k > out/kernel-ufswrite.log 2>&1 || true
K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
echo "kernel: $(stat -c '%s B' "$K")  errors: $(grep -c 'error:' out/kernel-ufswrite.log)"
echo -n "  marker: "; grep -a -c "no darwin on this phone" "$K" || true

# --- userland (var_store) --------------------------------------------------------------------
SKIP="kernel loader boot" GL=1 bash userland/build.sh alioth > out/build-userland-var.log 2>&1 || echo "userland rc=$?"
ls -la userland/root.apfs | awk '{print "  root.apfs "$5" B"}'
strings userland/root/bin/var_store 2>/dev/null | grep -c rufs_logdump | sed 's/^/  var_store has rufs_logdump: /'

# --- payload ---------------------------------------------------------------------------------
EPOCH=$(date +%s)
CMD="loader_return_after=240 epoch=$EPOCH sb_delay=8 touchscreen=1 buttons=1 touch_stat=5 cpufreq=1 bootcpu=7 cpus=8 adreno_soc=a650 adreno=21 adreno_skip=0 adreno_pause=0 adreno_keep=1 adreno_watch=10 ufs=2 ufs_write=1 var_store=1"
python3 xnuloader/mkpayload.py -o out/run-payload-persist.bin kernel=$K ramdisk=userland/root.apfs --cmdline "$CMD" | tail -1
