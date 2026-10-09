"""Check the Broadcom CFE 'vmlinux.lz4' format: 20-byte header + raw LZ4 block.

header (big endian): load_addr, entry, compressed_len, 'BRCM', uncompressed_len
usage: python -I brcm_lz4.py vmlinux.lz4 [out.bin]
"""
import struct
import sys


def lz4_block_decompress(src, out_len):
    dst = bytearray()
    i = 0
    while i < len(src):
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        dst += src[i:i + lit]
        i += lit
        if i >= len(src):
            break
        off = src[i] | (src[i + 1] << 8)
        i += 2
        mlen = token & 15
        if mlen == 15:
            while True:
                b = src[i]
                i += 1
                mlen += b
                if b != 255:
                    break
        mlen += 4
        start = len(dst) - off
        for k in range(mlen):
            dst.append(dst[start + k])
    return bytes(dst)


f = open(sys.argv[1], 'rb').read()
load, entry, clen, magic, ulen = struct.unpack('>III4sI', f[:20])
print(f'load {load:#010x} entry {entry:#010x} clen {clen} (file-20 = {len(f) - 20}) '
      f'magic {magic!r} ulen {ulen}')
out = lz4_block_decompress(f[20:20 + clen], ulen)
print(f'decompressed {len(out)} bytes, matches ulen: {len(out) == ulen}')
print('first words:', ' '.join(f'{w:08x}' for w in struct.unpack('>8I', out[:32])))
entry_off = entry - load
print(f'entry at offset {entry_off:#x}:',
      ' '.join(f'{w:08x}' for w in struct.unpack('>4I', out[entry_off:entry_off + 16])))
banner = out.find(b'Linux version')
print('banner:', out[banner:banner + 120].split(b'\n')[0].decode('latin-1') if banner >= 0 else None)
if len(sys.argv) > 2:
    open(sys.argv[2], 'wb').write(out)
