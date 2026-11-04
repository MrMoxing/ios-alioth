#!/bin/bash
K=~/k40/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
/usr/bin/llvm-objdump -d --start-address=0xfffffff0074ad11c --stop-address=0xfffffff0074ad260 "$K" 2>/dev/null | tail -60
