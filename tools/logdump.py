#!/usr/bin/env python3
"""Read the loader's log (and the store's header) straight off the phone's logdump.

The loader writes its log into logdump's first megabytes, so a run can be read back from Android
with root after the fact -- no `fastboot get_staged`, which is what used to fail whenever the
loader's USB side went quiet. Two things are asked of the phone:

    logdump.py log [out.bin]     dump the log and print the last session's interesting lines
    logdump.py head [offset]     the first bytes at an offset, to see whether a store is there

Offsets are bytes. The store starts at 8 MB (block 2048 of 4096).
"""
import subprocess
import sys

ADB = "adb"
DEV = "/dev/block/by-name/logdump"

INTERESTING = (
    "ufs:", "var_store", "the store", "the run is ending", "writes into", "REFUSED",
    "takes writes", "no darwin", "store", "slot", "var-save", "var_save",
)


def dump(path):
    out = subprocess.run(
        [ADB, "exec-out", f'su -c "dd if={DEV} bs=1M count=2 2>/dev/null"'],
        stdout=subprocess.PIPE, timeout=180,
    )
    data = out.stdout
    with open(path, "wb") as f:
        f.write(data)
    return data


def last_session(text):
    """The loader does not clear the log: take what follows the last banner."""
    lines = text.splitlines()
    start = 0
    for i, line in enumerate(lines):
        if "xnuloader" in line and "== " in line:
            start = i
    return lines[start:]


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else "log"
    if what == "head":
        offset = int(sys.argv[2]) if len(sys.argv) > 2 else 8 << 20
        block = offset // 4096
        out = subprocess.run(
            [ADB, "exec-out",
             f'su -c "dd if={DEV} bs=4096 skip={block} count=1 2>/dev/null"'],
            stdout=subprocess.PIPE, timeout=120,
        ).stdout
        print(f"{len(out)} bytes at {offset:#x}:")
        for row in range(0, min(len(out), 64), 16):
            chunk = out[row:row + 16]
            hexed = " ".join(f"{b:02x}" for b in chunk)
            text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
            print(f"  {offset + row:08x}  {hexed:<47}  {text}")
        magic = out[:6]
        print("magic:", magic, "->", "a store is here" if magic == b"RDWVAR" else "empty" if not any(out) else "something else")
        return

    path = sys.argv[2] if len(sys.argv) > 2 else "runs/logdump-last.bin"
    data = dump(path)
    text = data.decode("utf-8", "replace")
    session = last_session(text)
    print(f"{len(data)} bytes dumped to {path}; last session {len(session)} lines\n")
    for line in session:
        if any(key in line for key in INTERESTING):
            print(" ", line[:190])


if __name__ == "__main__":
    main()
