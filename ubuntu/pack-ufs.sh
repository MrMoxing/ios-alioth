#!/bin/bash
# Phase 0/1: build a payload that asks the loader for the UFS survey (ufs=1) or the block
# service (ufs=2). Read-only unless ufs_write=1 is also passed.
set -e
cd /home/mrmoxing/k40
K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
R=userland/root.apfs
TAG=${1:-ufs1}
shift || true
EXTRA="$*"
EPOCH=$(date +%s)
BASE="loader_return_after=200 epoch=$EPOCH sb_delay=8 touchscreen=1 buttons=1 touch_stat=5 cpufreq=1 bootcpu=7 cpus=8 adreno_soc=a650 adreno=21 adreno_skip=0 adreno_pause=0 adreno_keep=1 adreno_watch=10"
python3 xnuloader/mkpayload.py -o "out/run-payload-$TAG.bin" kernel=$K ramdisk=$R --cmdline "$BASE $EXTRA" | tail -2
ls -la "out/run-payload-$TAG.bin"
