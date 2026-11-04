#!/bin/bash
# Did the three artefacts actually pick up the logdump store?
echo "== loader.efi"
ls -la --time-style=+%H:%M /home/mrmoxing/k40/xnuloader/loader.efi | awk '{print "  "$5" B "$6}'
grep -a -c "no darwin on this phone" /home/mrmoxing/k40/xnuloader/loader.efi || echo "  0 (string missing)"
echo "== boot image"
grep -a -c "no darwin on this phone" /home/mrmoxing/k40/out/alioth-boot.img || echo "  0 (loader not inside?)"
echo "== kernel"
K=/home/mrmoxing/k40/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
ls -la --time-style=+%H:%M "$K" | awk '{print "  "$5" B "$6}'
grep -a -c "logdump" "$K" || echo "  0 (kernel has no logdump)"
echo "== var_store"
grep -a -c "rufs_logdump" /home/mrmoxing/k40/userland/bin/var_store 2>/dev/null || echo "  0"
echo "== build log lines"
grep -E "build: loader|build: out/|loader rc" /home/mrmoxing/k40/out/build-loader.log | head -5
