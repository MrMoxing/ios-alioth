#!/bin/bash
M=~/k40/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
for s in "vote for the PHY" "THE COMMAND DATABASE HAS NO" "the blocks were mapped before" "the nine blocks are mapped" "THE POWER VOTE GOT NO ANSWER" "ldoa9" "cx.lvl"; do
	printf '%s => ' "$s"
	strings "$M" | grep -c "$s"
done
