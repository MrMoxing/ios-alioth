#!/usr/bin/env python3
"""Diff the phone dt's qcom,phy-sequence triplets against the table in alioth_pcie.c."""
import re, struct, subprocess

ADB = "/mnt/d/Android/SDK/platform-tools/adb.exe"
P = "/sys/firmware/devicetree/base/soc/qcom,pcie@1c00000/qcom,phy-sequence"
r = subprocess.run([ADB, "exec-out", f'su -c "cat {P} 2>/dev/null"'],
                   stdout=subprocess.PIPE, timeout=60)
b = r.stdout.replace(b"\r\n", b"\n")
v = [struct.unpack(">I", b[i:i+4])[0] for i in range(0, len(b) - len(b) % 4, 4)]
dt = [(v[i], v[i+1], v[i+2]) for i in range(0, len(v) - len(v) % 3, 3)]

src = open(r"/mnt/e/Productivity/WorkBuudy/ios-alioth/src/hackios-xnu-kernel-main/xnu/files/bsd/dev/alioth_pcie.c").read()
m = re.search(r"phy_sequence\[\] = \{(.*?)\};", src, re.S)
ours = [(int(a, 16), int(b2, 16)) for a, b2 in re.findall(r"\{\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+)\s*\}", m.group(1))]

print(f"dt triplets: {len(dt)}, ours: {len(ours)}")
bad = 0
for i, (off, val, delay) in enumerate(dt):
    if i >= len(ours):
        print(f"  MISSING in ours: {off:#06x} = {val:#04x} delay {delay}")
        bad += 1
        continue
    roff, rval = ours[i]
    if roff != off or rval != val:
        print(f"  MISMATCH #{i}: dt {off:#06x}={val:#04x} delay {delay} vs ours {roff:#06x}={rval:#04x}")
        bad += 1
    if delay:
        print(f"  DELAY #{i}: {off:#06x} waits {delay} us after the write (ours: none)")
if len(ours) > len(dt):
    for off, val in ours[len(dt):]:
        print(f"  EXTRA in ours: {off:#06x} = {val:#04x}")
print("identical" if bad == 0 and len(ours) == len(dt) else f"{bad} mismatches")
