#!/bin/bash
# Probe the phone's live device tree for the TLMM tile layout.
ADB=/mnt/d/Android/SDK/platform-tools/adb.exe
$ADB shell 'su -c "ls /sys/firmware/devicetree/base/soc/pinctrl@f000000/"' > /tmp/tlmm.list
grep -iE "tile|south|north|east|reg-names" /tmp/tlmm.list
echo ====
for f in reg-names qcom,tiles-specs qcom,gpio-offset; do
  echo "-- $f:"
  $ADB shell "su -c 'cat /sys/firmware/devicetree/base/soc/pinctrl@f000000/$f 2>/dev/null'" | od -A d -t x1 | head -6
done
