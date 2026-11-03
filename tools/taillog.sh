#!/bin/bash
# Print the tail of the last text region inside a logdump image.
# Usage: taillog.sh <dump.bin> [bytes]
F="$1"
N="${2:-4000}"
python3 - "$F" "$N" <<'EOF'
import sys
data = open(sys.argv[1], 'rb').read().rstrip(b'\x00')
n = int(sys.argv[2])
print(f"text ends at {len(data):#x} ({len(data)}); showing last {n} bytes")
text = data[-n:].decode('utf-8', 'replace')
for line in text.splitlines():
    if line.strip():
        print(line[:220])
EOF
