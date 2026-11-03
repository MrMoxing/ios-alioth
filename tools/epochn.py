#!/usr/bin/env python3
"""Find a marker in a logdump image and print the lines after it."""
import re
import sys

path, marker = sys.argv[1], sys.argv[2].encode()
count = int(sys.argv[3]) if len(sys.argv) > 3 else 120
data = open(path, "rb").read()
pos = [m.start() for m in re.finditer(re.escape(marker), data)]
print(f"{len(pos)} hits for {marker!r}: {pos[-3:] if pos else ''}")
if not pos:
    sys.exit(1)
start = pos[-1]
tail = data[start:start + (200 << 10)]
lines = []
for raw in tail.split(b"\n"):
    text = raw.decode("ascii", "replace")
    text = "".join(c for c in text if 32 <= ord(c) < 127 or c == "\t")
    if text.strip():
        lines.append(text)
print(f"{len(lines)} lines after the hit:")
for l in lines[:count]:
    print(l)
