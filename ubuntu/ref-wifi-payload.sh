#!/bin/bash
# Boot the reference implementation's kernel on our own harness: their kernel (ath11k inside it),
# our root.apfs rebuilt with the QCA6390 firmware under the names their loader asks for, our loader
# and our system image.
#
#   fw/  qca6390_amss20.bin  qca6390_m3.bin  qca6390_bd_k11a.elf  qca6390_regdb.bin
#   args pcie_step=31 pcie_ipcr=2 wifi_lx=2 wifi_station=1  (hakios_wifi_option reads these)
set -e
REF=/home/mrmoxing/hakios-ref
SRC=/mnt/e/Productivity/WorkBuudy/ios-alioth/src/hackios-xnu-userland-main/userland
cd /home/mrmoxing/k40/userland

cp "$SRC/build.sh" build.sh
sed -i 's/\r$//' build.sh
cp "$SRC/rc.jobs.lang" rc.jobs.lang 2>/dev/null || true
rm -rf var && cp -r "$SRC/var" var

echo "== userland with the QCA6390 firmware (QCA=1)"
SKIP="kernel loader boot" GL=1 QCA=1 RC_JOBS=rc.jobs bash build.sh alioth > ../out/scene-refwifi.log 2>&1
grep -c "error:" ../out/scene-refwifi.log | sed 's/^/  errors: /' || true
stat -c '  root.apfs: %s B' root.apfs

echo "== the payload: their kernel, our images"
K=$REF/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
ls -l "$K" | sed 's/^/  /'
EPOCH=$(date +%s)
# JOIN=1: the station joins the network named in fw/network by itself (wifi_join=1, hakios.radio.c)
# and stays joined for HOLD seconds; without it the station is offered to iOS and Settings joins.
WIFI="wifi_station=1 wifi_keep=1"
[ -n "$JOIN" ] && WIFI="wifi_join=1 wifi_hold=${HOLD:-120}"
python3 /home/mrmoxing/k40/xnuloader/mkpayload.py -o /mnt/e/Productivity/WorkBuudy/ios-alioth/boot/run-payload-refwifi.bin \
	kernel="$K" ramdisk=root.apfs \
	--cmdline "loader_return_after=${LRA:-180} epoch=$EPOCH sb_delay=8 touchscreen=1 buttons=1 touch_stat=5 cpufreq=1 bootcpu=7 cpus=8 adreno_soc=a650 adreno=21 adreno_skip=0 adreno_pause=0 adreno_keep=1 adreno_watch=10 ufs=2 ufs_write=1 var_store=1 pcie_step=31 pcie_at=2 pcie_ipcr=2 wifi_lx=2 wifi_quiet=1 $WIFI wifi=1 wifi_mac=6c:f7:84:8b:81:b0"
echo "epoch=$EPOCH"
ls -l /mnt/e/Productivity/WorkBuudy/ios-alioth/boot/run-payload-refwifi.bin | sed 's/^/  /'
