#!/usr/bin/env python3
"""Take the shipped alioth boot image apart (Android boot header v1): the kernel section is
gzip(BootShim + UEFI FD + payload) and the DTB rides in the same section after the gzip stream.
Extracted so mkxnuimg.py can repack with a new loader payload without the Mu-Silicium build tree."""
import struct, sys, os, zlib

src, outdir = sys.argv[1], sys.argv[2]
fd_path = sys.argv[3] if len(sys.argv) > 3 else None
os.makedirs(outdir, exist_ok=True)
d = open(src, 'rb').read()
assert d[:8] == b'ANDROID!', d[:8]
kernel_size, kernel_addr, ramdisk_size, ramdisk_addr, second_size, second_addr, \
    tags_addr, page_size, header_version, os_version = struct.unpack_from('<10I', d, 8)
print('header v%d: kernel %d ramdisk %d second %d page %d' %
      (header_version, kernel_size, ramdisk_size, second_size, page_size))

pos = page_size
kernel = d[pos:pos + kernel_size]
blob, dtb = kernel, b''
if kernel[:2] == b'\x1f\x8b':
    dz = zlib.decompressobj(31)
    blob = dz.decompress(kernel) + dz.flush()
    dtb = dz.unused_data
    print('gzip stream ends at %d of %d; tail (DTB) %d bytes' % (len(kernel) - len(dtb), len(kernel), len(dtb)))

open(os.path.join(outdir, 'kernel.gz'), 'wb').write(kernel)
if dtb:
    open(os.path.join(outdir, 'alioth.dtb'), 'wb').write(dtb)
print('blob %d bytes' % len(blob))
if fd_path:
    fd = open(fd_path, 'rb').read()
    i = blob.find(fd[:64])
    print('FD found at %d' % i if i >= 0 else 'FD NOT found')
    if i >= 0:
        open(os.path.join(outdir, 'alioth-shim.bin'), 'wb').write(blob[:i])
        print('shim %d bytes, FD %d bytes, payload %d bytes' % (i, len(fd), len(blob) - i - len(fd)))
