#!/bin/bash
# Dump the phone's rsc@18200000 (and its drv children) node from the live dt.
ADB=/mnt/d/Android/SDK/platform-tools/adb.exe
$ADB shell 'su -c "ls /sys/firmware/devicetree/base/soc/"' > /tmp/soc.list
grep -iE "rsc|rpmh" /tmp/soc.list
echo ====
for p in /sys/firmware/devicetree/base/soc/rsc@18200000; do
  $ADB shell "su -c 'ls $p'" 2>/dev/null
  echo ----
  for f in qcom,tcs-offset qcom,drv-id qcom,tcs-config reg; do
    printf '%s: ' "$f"
    $ADB shell "su -c 'cat $p/$f 2>/dev/null'" | od -A n -t x1 | tr -d '\n' | sed 's/ *$//'
    echo
  done
done
