#!/bin/bash
ADB=/mnt/d/Android/SDK/platform-tools/adb.exe
P=/sys/firmware/devicetree/base/soc/qcom,pcie@1c00000
$ADB shell "su -c 'ls $P'" 2>/dev/null | tr '\r' ' '
echo "---- values"
for f in qcom,vreg-1p8-config qcom,vreg-0p9-config qcom,vreg-cx-config vreg-1p8-supply vreg-0p9-supply vreg-cx-supply qcom,perst-delay-us-min qcom,perst-delay-us-max qcom,ep-latency-us qcom,slv-addr-space-size qcom,phy-status-offset qcom,phy-status-bit qcom,phy-sequence-len qcom,phy-sequence qcom,clk-write-cmd-status-offsets perst-gpio wake-gpio; do
  printf '%s: ' "$f"
  $ADB shell "su -c 'cat $P/$f 2>/dev/null'" | od -A n -t x1 | tr -d '\n' | sed 's/ *$//'
  echo
done
