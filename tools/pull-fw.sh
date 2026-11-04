#!/bin/bash
# Pull the QCA6390 WLAN firmware off the phone into the tree (fw-qca/).
set -e
ADB=/mnt/d/Android/SDK/platform-tools/adb.exe
OUT=/mnt/e/Productivity/WorkBuudy/ios-alioth/fw-qca
mkdir -p "$OUT"
for f in amss20.bin m3.bin regdb.bin bdwlan.elf; do
  $ADB exec-out "su -c 'cat /vendor/firmware_mnt/image/qca6390/$f'" > "$OUT/$f"
  ls -l "$OUT/$f"
  md5sum "$OUT/$f"
done
