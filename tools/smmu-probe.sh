#!/bin/bash
ADB=/mnt/d/Android/SDK/platform-tools/adb.exe
P=/sys/firmware/devicetree/base/soc/apps-smmu@15000000
$ADB shell "su -c 'ls $P'" 2>/dev/null | tr '\r' ' ' | head -30
echo "---- values"
for f in clock-names clocks reg; do
  printf '%s: ' "$f"
  $ADB exec-out "su -c 'cat $P/$f 2>/dev/null'" | od -A n -t x1 | tr -d '\n' | sed 's/ *$//'
  echo
done
