#!/bin/bash
# The pcie0 node's iommu facts: iommu-map, qcom,smmu-sid-base, dma-coherent; and the apps smmu node.
ADB=/mnt/d/Android/SDK/platform-tools/adb.exe
P=/sys/firmware/devicetree/base/soc/qcom,pcie@1c00000
for f in iommu-map qcom,smmu-sid-base dma-coherent iommu-map-mask; do
  printf '%s: ' "$f"
  $ADB exec-out "su -c 'cat $P/$f 2>/dev/null'" | od -A n -t x1 | tr -d '\n' | sed 's/ *$//'
  echo
done
echo "==== apps smmu node:"
$ADB shell 'su -c "ls /sys/firmware/devicetree/base/soc/"' | tr -d '\r' | grep -iE "smmu" | head -10
for n in apps_smmu smmu_mm500 smmu-500; do
  $ADB exec-out "su -c 'cat /sys/firmware/devicetree/base/soc/$n/compatible 2>/dev/null'" | tr -d '\r\n' | sed "s/^/$n: /"; echo
done
