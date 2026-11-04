#!/bin/bash
# Find the QCA6390 WLAN firmware files on the phone.
ADB=/mnt/d/Android/SDK/platform-tools/adb.exe
$ADB shell 'su -c "ls /vendor/firmware/ 2>/dev/null"' | tr -d '\r' | grep -iE "qca|wlan|amss|m3|bdwlan|bdb"
echo "==== qca6390 dir:"
$ADB shell 'su -c "ls /vendor/firmware/qca6390/ 2>/dev/null"' | tr -d '\r'
echo "==== other dirs:"
$ADB shell 'su -c "ls /vendor/firmware_mnt/image/ 2>/dev/null"' | tr -d '\r' | grep -iE "wlan|qca" | head -20
$ADB shell 'su -c "ls /vendor/firmware/wlan/ 2>/dev/null"' | tr -d '\r' | head -20
