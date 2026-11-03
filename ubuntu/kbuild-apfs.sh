#!/bin/bash
# overlay -> xnu-src, force this file's object to rebuild, build, repack.
#   kbuild-apfs.sh <payload-tag> [extra cmdline...]
set -e
cd /home/mrmoxing/k40
TAG=${1:?tag}
shift
EXTRA="$*"
# apply.sh copies xnu/files over Apple's sources (the overlay is the authority)
bash xnu/apply.sh > out/apply.log 2>&1 || echo "apply: rc=$?"
O=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT
rm -f "$O/apfs_vfsops.o" "$O/redwood_ufs.o"
DEVICE=alioth ./xnu-build.sh -j16 -k > out/kernel-apfs.log 2>&1 || true
K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
echo "kernel: $(stat -c '%s B %y' "$K" | cut -c1-40)"
echo -n "refresh in kernel: "; grep -a -c "root volume answers" "$K" || true
EPOCH=$(date +%s)
BASE="loader_return_after=200 epoch=$EPOCH sb_delay=8 touchscreen=1 buttons=1 touch_stat=5 cpufreq=1 bootcpu=7 cpus=8 adreno_soc=a650 adreno=21 adreno_skip=0 adreno_pause=0 adreno_keep=1 adreno_watch=10"
python3 xnuloader/mkpayload.py -o "out/run-payload-$TAG.bin" kernel=$K ramdisk=userland/root.apfs --cmdline "$BASE $EXTRA" | tail -1
ls -la "out/run-payload-$TAG.bin"
