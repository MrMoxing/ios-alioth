#!/bin/bash
# p38: pcie=14 -- the same SBL transfer as p37, with the two things p37 could not answer:
#   - the SBL now comes from a buffer of our own (kmem_alloc_contig) instead of the RAM disk image,
#     so the address handed to the PBL is one we chose and report;
#   - BHI_IMGADDR/IMGSIZE are read back after the write, one trigger only, and the endpoint's
#     config space is read after the failure to tell a reset function from a device that left the bus.
set -e
cd /home/mrmoxing/k40
cp /mnt/e/Productivity/WorkBuudy/ios-alioth/src/hackios-xnu-kernel-main/xnu/files/bsd/dev/alioth_pcie.c \
   xnu-src/xnu-8796.121.2/bsd/dev/alioth_pcie.c
rm -f xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/bsd/dev/alioth_pcie.o
DEVICE=alioth ./xnu-build.sh > /tmp/b-p38.log 2>&1 || true
grep -E "error:" /tmp/b-p38.log | head -10 || echo "no errors"
EPOCH=$(date +%s)
python3 xnuloader/mkpayload.py -o /mnt/e/Productivity/WorkBuudy/ios-alioth/boot/run-payload-p38.bin \
	kernel=/home/mrmoxing/k40/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 \
	ramdisk=/home/mrmoxing/k40/userland/root.apfs \
	--cmdline "loader_return_after=240 epoch=$EPOCH sb_delay=8 touchscreen=1 buttons=1 touch_stat=5 cpufreq=1 bootcpu=7 cpus=8 adreno_soc=a650 adreno=21 adreno_skip=0 adreno_pause=0 adreno_keep=1 adreno_watch=10 ufs=2 ufs_write=1 var_store=1 pcie=14 pcie_thread=1 pcie_delay=3000 pcie_pause=100 pcie_wait=30000"
echo "epoch=$EPOCH"
