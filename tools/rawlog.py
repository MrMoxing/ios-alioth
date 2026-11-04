#!/usr/bin/env python3
"""Extract text lines from a logdump image and show the last session's tail."""
import re
import sys

path = sys.argv[1]
data = open(path, "rb").read()
# find the last loader banner
banners = [m.start() for m in re.finditer(rb"XNULOADER", data)]
print(f"{len(data)} bytes, {len(banners)} banners")
if not banners:
    sys.exit(1)
start = banners[-1]
tail = data[start:start + 4 << 20]
# split into lines, keep printable
lines = []
for raw in tail.split(b"\n"):
    text = raw.decode("ascii", "replace")
    text = "".join(c for c in text if 32 <= ord(c) < 127 or c == "\t")
    if text.strip():
        lines.append(text)
print(f"last session: {len(lines)} lines from byte {start:#x}")
what = sys.argv[2] if len(sys.argv) > 2 else ""
n = int(sys.argv[3]) if len(sys.argv) > 3 else 60
if what == "-i":
    pat = re.compile(sys.argv[4], re.I) if len(sys.argv) > 4 else None
    hits = [l for l in lines if pat and pat.search(l)]
    print(f"{len(hits)} matching lines:")
    for l in hits[-n:]:
        print(l)
else:
    for l in lines[-n:]:
        print(l)
