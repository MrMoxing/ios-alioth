#!/bin/bash
# p16: same as p15 (pcie=5 pcie_thread=1 pcie_delay=15000) but pcie_skip=0xFFE skips
#   every step, so the thread runs nothing but its announcements. Discriminates:
#   - survives              -> thread start is fine, the killer is one of the steps
#   - dies, "the %u ms delay is over" in the log    -> kernel_thread_start kills
#   - dies, no such line    -> the death is in the tail of the delay sleep itself
set -e
cd /home/mrmoxing/k40
EPOCH=$(date +%s)
python3 xnuloader/mkpayload.py -o /mnt/e/Productivity/WorkBuudy/ios-alioth/boot/run-payload-p16.bin \
	kernel=/home/mrmoxing/k40/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 \
	ramdisk=/home/mrmoxing/k40/userland/root.apfs \
	--cmdline "loader_return_after=240 epoch=$EPOCH sb_delay=8 touchscreen=1 buttons=1 touch_stat=5 cpufreq=1 bootcpu=7 cpus=8 adreno_soc=a650 adreno=21 adreno_skip=0 adreno_pause=0 adreno_keep=1 adreno_watch=10 ufs=2 ufs_write=1 var_store=1 lang_probe=1 pcie=5 pcie_skip=0xFFE pcie_thread=1 pcie_delay=15000"
python3 /mnt/e/Productivity/WorkBuudy/ios-alioth/boot/inspect_payload.py /mnt/e/Productivity/WorkBuudy/ios-alioth/boot/run-payload-p16.bin
echo "epoch=$EPOCH"
