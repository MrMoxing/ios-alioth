#!/bin/bash
# Disassemble alioth_pcie's thread and map_all in the shipped kernel to see
# whether pcie_steps_thread still calls map_all (the only 2MB ml_io_map).
set -e
K=~/k40/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
OBJDUMP=$(command -v llvm-objdump || command -v objdump || ls ~/k40/toolchain*/bin/llvm-objdump 2>/dev/null | head -1)
echo "objdump: $OBJDUMP"
"$OBJDUMP" -t "$K" 2>/dev/null | grep -i alioth_pcie | head -40
