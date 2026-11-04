#!/bin/bash
# p25: pcie=11 (no skip) pcie_thread=1 pcie_delay=3000 -- the full sequence including step 11:
#   PERST out, the vendor's training words, LTSSM on, the three link checks. p24 got the PHY
#   ready (status 0xa6, pipe clock 62.4 MHz). Look for "THE LINK IS UP" and the endpoint's
#   vendor/device id (0x17cb / 0x0101) in the poll lines.
set -e
cd /home/mrmoxing/k40
EPOCH=$(date +%s)
python3 xnuloader/mkpayload.py -o /mnt/e/Productivity/WorkBuudy/ios-alioth/boot/run-payload-p25.bin \
	kernel=/home/mrmoxing/k40/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 \
	ramdisk=/home/mrmoxing/k40/userland/root.apfs \
	--cmdline "loader_return_after=240 epoch=$EPOCH sb_delay=8 touchscreen=1 buttons=1 touch_stat=5 cpufreq=1 bootcpu=7 cpus=8 adreno_soc=a650 adreno=21 adreno_skip=0 adreno_pause=0 adreno_keep=1 adreno_watch=10 ufs=2 ufs_write=1 var_store=1 lang_probe=1 pcie=11 pcie_thread=1 pcie_delay=3000"
python3 /mnt/e/Productivity/WorkBuudy/ios-alioth/boot/inspect_payload.py /mnt/e/Productivity/WorkBuudy/ios-alioth/boot/run-payload-p25.bin
echo "epoch=$EPOCH"
