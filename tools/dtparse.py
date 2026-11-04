#!/usr/bin/env python3
"""Parse properties of the phone's qcom,pcie@1c00000 node out of the live dt."""
import subprocess, struct, sys

ADB = "/mnt/d/Android/SDK/platform-tools/adb.exe"
P = "/sys/firmware/devicetree/base/soc/qcom,pcie@1c00000"

def cat(name):
    r = subprocess.run([ADB, "shell", f'su -c "cat {P}/{name} 2>/dev/null"'],
                       stdout=subprocess.PIPE, timeout=60)
    return r.stdout.replace(b"\r\n", b"").replace(b"\n", b"")

def u32s(b):
    return [struct.unpack(">I", b[i:i+4])[0] for i in range(0, len(b) - len(b) % 4, 4)]

def strings(b):
    return [x.decode() for x in b.split(b"\x00") if x]

props = ["compatible", "reg", "reg-names", "clock-names", "resets", "reset-names",
         "qcom,pcie-phy-ver", "qcom,phy-power-down-offset", "qcom,use-19p2mhz-aux-clk",
         "qcom,boot-option", "qcom,ep-latency", "qcom,slv-addr-space-size",
         "qcom,phy-status-offset", "qcom,phy-status-bit", "qcom,drv-supported",
         "max-clock-frequency-hz", "qcom,vreg-1p8-voltage-level",
         "qcom,vreg-0p9-voltage-level", "qcom,vreg-cx-voltage-level",
         "qcom,l1-2-th-value", "qcom,l1-2-th-scale", "gdsc-vdd-supply",
         "perst-gpio", "wake-gpio", "cell-index"]
for p in props:
    b = cat(p)
    if not b:
        print(f"{p}: <none>")
    elif p in ("compatible", "reg-names", "clock-names", "reset-names"):
        print(f"{p}: {strings(b)}")
    elif p == "qcom,phy-sequence":
        v = u32s(b)
        print(f"qcom,phy-sequence: {len(v)} words = {len(v)//3} triplets (offset value delay)")
        for i in range(0, len(v) - len(v) % 3, 3):
            print(f"  {v[i]:#06x} {v[i+1]:#04x} delay {v[i+2]}")
    else:
        print(f"{p}: {u32s(b)}")
