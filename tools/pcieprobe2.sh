#!/bin/bash
ADB=/mnt/d/Android/SDK/platform-tools/adb.exe
$ADB shell 'su -c "ls /sys/firmware/devicetree/base/soc/"' > /tmp/soc.list
grep -iE "pcie|pci" /tmp/soc.list
echo "==== qcom,pcie compatibles"
grep -iE "pcie_phy|pciephy" /tmp/soc.list
