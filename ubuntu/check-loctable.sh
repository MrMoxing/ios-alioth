#!/bin/bash
# What languages a table in the system image holds: extract it, then say.
#   check-loctable.sh /System/Library/.../Something.loctable
set -e
cd /home/mrmoxing/k40
python3 tools/apfs/apfs.py cat userland/system.apfs "$1" > /tmp/lt.bin
python3 /mnt/e/Productivity/WorkBuudy/ios-alioth/tools/loctable-langs.py /tmp/lt.bin
