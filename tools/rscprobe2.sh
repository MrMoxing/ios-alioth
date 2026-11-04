#!/bin/bash
# Try to read the DRV's ncpt / TCS info from Android's rpmh debugfs.
ADB=/mnt/d/Android/SDK/platform-tools/adb.exe
$ADB shell 'su -c "ls /sys/kernel/debug/rpmh 2>/dev/null || ls /d/rpmh 2>/dev/null"'
echo ====
$ADB shell 'su -c "find /sys/kernel/debug -maxdepth 2 -iname \"*rpmh*\" 2>/dev/null"'
