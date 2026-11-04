#!/bin/bash
ADB=/mnt/d/Android/SDK/platform-tools/adb.exe
echo "== /vendor/firmware/wlan/:"
$ADB shell 'su -c "ls -la /vendor/firmware/wlan/ 2>/dev/null"' | tr -d '\r'
echo "== /vendor/firmware_mnt/image/qca6390/:"
$ADB shell 'su -c "ls -la /vendor/firmware_mnt/image/qca6390/ 2>/dev/null"' | tr -d '\r'
echo "== /vendor/firmware_mnt/image/qca_cld/:"
$ADB shell 'su -c "ls -la /vendor/firmware_mnt/image/qca_cld/ 2>/dev/null"' | tr -d '\r' | head -20
echo "== find amss/m3 anywhere in vendor:"
$ADB shell 'su -c "find /vendor -iname \"*amss*\" -o -iname \"*m3.bin\" -o -iname \"*qdss*\" 2>/dev/null"' | tr -d '\r' | head -20
