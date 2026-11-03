#!/bin/bash
K=~/k40/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
OBJDUMP=/usr/bin/llvm-objdump
"$OBJDUMP" -t "$K" 2>/dev/null | grep -E 'pcie_steps_thread|run_steps|map_all|step_power|cmd_db_address|rpmh_write' | head -20
echo ====
# disassemble the init to find the thread function's address (kernel_thread_start's second arg)
"$OBJDUMP" -d --start-address=0xfffffff0074accf4 --stop-address=0xfffffff0074ae000 "$K" 2>/dev/null | grep -nE 'kernel_thread_start|bl\s' | head -60
