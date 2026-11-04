#!/bin/bash
# Grep the last-session pcie lines out of a logdump image.
# Usage: greplog.sh <dump.bin> [pattern] [tail]
F="$1"
P="${2:-pcie: 1[23]:}"
N="${3:-60}"
python3 - "$F" "$P" "$N" <<'EOF'
import re, sys
data = open(sys.argv[1], 'rb').read().rstrip(b'\x00')
text = data.decode('utf-8', 'replace')
lines = [l for l in text.splitlines() if re.search(sys.argv[2], l)]
print('\n'.join(l[:150] for l in lines[-int(sys.argv[3]):]))
EOF
