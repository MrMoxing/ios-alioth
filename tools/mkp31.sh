#!/bin/bash
# p31: pcie=0 (no pcie steps at all) with the NEW root.apfs (fw files inside) -- the control:
#   if this dies around 24-26 s too, the rebuilt image is the killer, not the pcie steps.
set -e
cd /home/mrmoxing/k40
EPOCH=$(date +%s)
python3 xnuloader/mkpayload.py -o /mnt/e/Productivity/WorkBuudy/ios-alioth/boot/run-payload-p31.bin \
	kernel=/home/mrmoxing/k40/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 \
	ramdisk=/home/mrmoxing/k40/userland/root.apfs \
	--cmdline "loader_return_after=240 epoch=$EPOCH sb_delay=8 touchscreen=1 buttons=1 touch_stat=5 cpufreq=1 bootcpu=7 cpus=8 adreno_soc=a650 adreno=21 adreno_skip=0 adreno_pause=0 adreno_keep=1 adreno_watch=10 ufs=2 ufs_write=1 var_store=1 lang_probe=1 pcie=0 pcie_thread=1 pcie_delay=3000"
python3 /mnt/e/Productivity/WorkBuudy/ios-alioth/boot/inspect_payload.py /mnt/e/Productivity/WorkBuudy/ios-alioth/boot/run-payload-p31.bin
echo "epoch=$EPOCH"
