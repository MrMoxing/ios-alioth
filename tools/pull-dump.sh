#!/bin/bash
# Pull the loader's log off the phone's logdump (first 2 MB) and save it.
# Usage: pull-dump.sh <out.bin> [megabytes]
set -e
ADB=/mnt/d/Android/SDK/platform-tools/adb.exe
OUT="$1"
MB="${2:-2}"
$ADB wait-for-device
$ADB exec-out "su -c 'dd if=/dev/block/by-name/logdump bs=1M count=$MB 2>/dev/null'" > "$OUT"
ls -l "$OUT"
