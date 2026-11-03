#!/bin/bash
# Dump the phone's rpmh-regulator-ldoa9/ldoa5/cxlvl nodes: compatible, reg, regulator-min/max.
ADB=/mnt/d/Android/SDK/platform-tools/adb.exe
P=/sys/firmware/devicetree/base/soc/rsc@18200000
for n in ldoa9 ldoa5 cxlvl; do
  echo "== $n"
  $ADB shell "su -c 'ls $P/rpmh-regulator-$n'" 2>/dev/null | tr '\r' ' '
  for f in compatible reg regulator-name regulator-min-microvolt regulator-max-microvolt qcom,regulator-initial-voltage regulator-initial-mode; do
    printf '%s: ' "$f"
    $ADB shell "su -c 'cat $P/rpmh-regulator-$n/$f 2>/dev/null'" | od -A n -t x1 | tr -d '\n' | sed 's/ *$//'
    echo
  done
done
