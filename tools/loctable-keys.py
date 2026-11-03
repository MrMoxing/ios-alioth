#!/usr/bin/env python3
"""What a .loctable holds for two languages, side by side.

    loctable-keys.py FILE.loctable [zh_CN] [en]

A table of tables: language -> table name -> string (the table name is the key). This prints, for
names both languages have, the two values -- which is what a lookup at run time has to return.
"""
import plistlib
import sys


def main():
    path = sys.argv[1]
    want = (sys.argv[2] if len(sys.argv) > 2 else "zh_CN",
            sys.argv[3] if len(sys.argv) > 3 else "en")
    table = plistlib.load(open(path, "rb"))
    for lang in want:
        if lang not in table:
            print(f"{lang}: absent (has {sorted(table)[:10]})")
            return
        print(f"{lang}: tables {list(table[lang])[:10]}")
    a, b = (table[l] for l in want)
    shared = [t for t in a if t in b]
    print(f"keys in both: {len(shared)} of {len(a)} / {len(b)}")
    shown = 0
    for name in shared:
        if a[name] != b[name]:
            print(f"  {name}: {want[0]}={a[name]!r} {want[1]}={b[name]!r}")
            shown += 1
            if shown >= 6:
                return


if __name__ == "__main__":
    main()
