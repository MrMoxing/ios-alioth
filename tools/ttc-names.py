#!/usr/bin/env python3
"""List the faces of a TrueType collection: family, style and PostScript name.

`ttcf` files hold several faces behind one path, and CoreText asks for one of them by its
PostScript name -- "Requested font not available from ...#postscript-name=.PingFangSC-Medium" says
the collection was opened and that name was not in it. This answers that directly, without a
font library.

    ttc-names.py FONT.ttc [--weights]
"""
import struct
import sys


def names_of(font_offset, data):
    num_tables = struct.unpack_from(">H", data, font_offset + 4)[0]
    name_off = None
    for i in range(num_tables):
        rec = font_offset + 12 + 16 * i
        tag, _checksum, offset, _length = struct.unpack_from(">4sIII", data, rec)
        if tag == b"name":
            name_off = offset
            break
    if name_off is None:
        return {}
    _fmt, count, string_off = struct.unpack_from(">HHH", data, name_off)
    found = {}
    for i in range(count):
        rec = name_off + 6 + 12 * i
        platform, encoding, language, name_id, length, offset = struct.unpack_from(">HHHHHH", data, rec)
        raw = data[name_off + string_off + offset:name_off + string_off + offset + length]
        if platform == 3 or (platform == 0):
            text = raw.decode("utf-16-be", "replace")
        else:
            text = raw.decode("mac-roman", "replace")
        if name_id in (1, 2, 6) and name_id not in found:
            found[name_id] = text
    return found


def main():
    path = sys.argv[1]
    data = open(path, "rb").read()
    if data[:4] != b"ttcf":
        print("not a collection:", data[:4])
        return
    _tag, _version, count = struct.unpack_from(">4sII", data, 0)
    print(f"{count} faces in {path}")
    offsets = struct.unpack_from(f">{count}I", data, 12)
    for n, off in enumerate(offsets):
        names = names_of(off, data)
        print(f"  {n:2d}  family={names.get(1, '?'):<24} style={names.get(2, '?'):<18} ps={names.get(6, '?')}")


if __name__ == "__main__":
    main()
