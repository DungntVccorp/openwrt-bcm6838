"""Minimal big-endian JFFS2 extractor (read-only).

usage: python -I jffs2x.py IMAGE START END OUTDIR
Prints the directory entries (with versions) and writes the latest version of
every file to OUTDIR.
"""
import lzma
import os
import struct
import sys
import zlib

img, start, end, out = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0), sys.argv[4]
data = open(img, 'rb').read()[start:end]
os.makedirs(out, exist_ok=True)

DIRENT, INODE = 0xE001, 0xE002
dirents = {}   # name -> (version, ino)
inodes = {}    # ino -> list of (version, offset, dsize, raw)
comprs = set()

pos = 0
while pos + 12 <= len(data):
    magic, ntype, totlen = struct.unpack_from('>HHI', data, pos)
    if magic != 0x1985 or totlen < 12 or pos + totlen > len(data):
        pos += 4
        continue
    if ntype == DIRENT:
        pino, ver, ino, mctime, nsize, typ = struct.unpack_from('>IIIIBB', data, pos + 12)
        name = data[pos + 40:pos + 40 + nsize].decode('latin-1')
        if name not in dirents or ver > dirents[name][0]:
            dirents[name] = (ver, ino)
    elif ntype == INODE:
        (ino, ver, mode, uid, gid, isize, atime, mtime, ctime, off, csize, dsize,
         compr, ucompr, flags) = struct.unpack_from('>IIIHHIIIIIIIBBH', data, pos + 12)
        raw = data[pos + 68:pos + 68 + csize]
        comprs.add(compr)
        if compr == 0:
            payload = raw
        elif compr == 6:
            payload = zlib.decompress(raw)
        elif compr == 8:     # JFFS2_COMPR_LZMA
            payload = lzma.decompress(raw, format=lzma.FORMAT_RAW,
                                      filters=[{'id': lzma.FILTER_LZMA1,
                                                'dict_size': 1 << 16, 'lc': 0, 'lp': 0, 'pb': 0}])
        elif compr == 1:     # zero
            payload = b'\0' * dsize
        else:
            payload = None
        inodes.setdefault(ino, []).append((ver, off, dsize, payload, isize, compr))
    pos += (totlen + 3) & ~3

print('compression types:', sorted(comprs))
for name, (ver, ino) in sorted(dirents.items()):
    if ino == 0:
        print(f'  {name:16} v{ver:<4} (deleted)')
        continue
    nodes = sorted(inodes.get(ino, []), key=lambda n: n[0])
    isize = max((n[4] for n in nodes), default=0)
    buf = bytearray(isize)
    missing = False
    for ver_, off, dsize, payload, _, compr in nodes:
        if payload is None:
            missing = True
            continue
        buf[off:off + len(payload)] = payload[:dsize]
    with open(os.path.join(out, name), 'wb') as f:
        f.write(buf)
    print(f'  {name:16} v{ver:<4} ino {ino:<3} size {isize:8}{"  (unsupported compression!)" if missing else ""}')
