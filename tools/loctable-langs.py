#!/usr/bin/env python3
"""Which languages a .loctable holds.

iOS 16 keeps an application's translations in one table of tables rather than a .lproj directory a
language, so "the UI is English although AppleLanguages says Chinese" comes down to whether the
table has Chinese at all. The file begins with a plist; when it parses, its keys are the languages.

    loctable-langs.py FILE.loctable
"""
import plistlib
import re
import sys


def main():
    data = open(sys.argv[1], "rb").read()
    print(f"{len(data)} bytes, starts {data[:8]!r}")
    try:
        value = plistlib.loads(data)
    except Exception as error:
        print("not a plist:", error)
        found = sorted({m.decode() for m in re.findall(rb"(?:zh|en|ja|ko|fr|de|es)[-_][A-Za-z]{2,4}", data)})
        print("language-looking strings:", found[:20])
        return
    keys = list(value)
    print(f"plist, {len(keys)} top-level keys")
    print("first:", keys[:8])
    langs = [k for k in keys if re.fullmatch(r"[a-z]{2,3}([-_][A-Za-z]{2,4})?", str(k))]
    print("language keys:", langs[:24])


if __name__ == "__main__":
    main()
