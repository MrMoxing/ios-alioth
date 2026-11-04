#!/bin/bash
# Show the logdump text around a pattern: N bytes before and after the LAST match.
# Usage: ctx.sh <dump.bin> <pattern> [before] [after]
F="$1"; P="$2"; B="${3:-400}"; A="${4:-1600}"
python3 - "$F" "$P" "$B" "$A" <<'EOF'
import re, sys
data = open(sys.argv[1], 'rb').read().rstrip(b'\x00')
text = data.decode('utf-8', 'replace')
ms = list(re.finditer(sys.argv[2], text))
if not ms:
    print("no match"); sys.exit()
m = ms[-1]
s = max(0, m.start() - int(sys.argv[3]))
chunk = text[s:m.start() + int(sys.argv[4])]
for l in chunk.splitlines():
    if l.strip():
        print(l[:170])
EOF
