"""Pack the translated shaders into one file: <dir>/shaders.pack, read by src/gfx/vkb_shader.c.

usage: python tools/pack_shaders.py [<dir>]   (default data\\shaders\\ryujinx)

The translation writes two files per shader, <key>.spv and <key>.res.json (~57,000 files for 1.4.3); the game reads
the pack first and falls back to them. The loose files stay: the next translation run (--new-only) adds to them, and
the pack is written again. Layout, little-endian: b'UTSHPK01', u32 count, u32 0, then count entries sorted by key
{key[32] (the SHA-256), u64 offset, u32 SPIR-V bytes, u32 reflection bytes}, then the data (each SPIR-V followed by
its reflection).
"""
import os
import pathlib
import re
import struct
import sys

MAGIC = b'UTSHPK01'
KEY = re.compile(r'[0-9a-f]{64}')


def pack(folder, out=None):
    folder = pathlib.Path(folder)
    out = pathlib.Path(out) if out else folder / 'shaders.pack'
    keys = sorted(p.stem for p in folder.glob('*.spv')
                  if KEY.fullmatch(p.stem) and (folder / f'{p.stem}.res.json').is_file())
    tmp = out.with_name(out.name + '.tmp')
    offset = 16 + 48 * len(keys)
    index = []
    with open(tmp, 'wb') as f:
        f.write(MAGIC + struct.pack('<II', len(keys), 0))
        f.write(bytes(48 * len(keys)))   # the index, written once the offsets are known
        for key in keys:
            spv = (folder / f'{key}.spv').read_bytes()
            refl = (folder / f'{key}.res.json').read_bytes()
            f.write(spv)
            f.write(refl)
            index.append(bytes.fromhex(key) + struct.pack('<QII', offset, len(spv), len(refl)))
            offset += len(spv) + len(refl)
        f.seek(16)
        f.write(b''.join(index))
    os.replace(tmp, out)
    return len(keys), offset


def stale(folder, out=None):
    """True if the pack is missing or its keys are not the translated ones (the translation added one). A shader
    rewritten in place is not seen: tools/setup.py packs again after a full translation. One listing and the pack's
    index only, no stat per file: exFAT has no directory index, and ~57,000 lookups took a minute cold."""
    folder = pathlib.Path(folder)
    out = pathlib.Path(out) if out else folder / 'shaders.pack'
    try:
        with open(out, 'rb') as f:
            head = f.read(16)
            if head[:8] != MAGIC:
                return True
            count = struct.unpack('<I', head[8:12])[0]
            index = f.read(48 * count)
    except OSError:
        return True
    packed = {index[i:i + 32].hex() for i in range(0, len(index), 48)}
    names = set(os.listdir(folder))
    keys = {n[:-4] for n in names if n.endswith('.spv') and KEY.fullmatch(n[:-4]) and n[:-4] + '.res.json' in names}
    return keys != packed


def main():
    folder = sys.argv[1] if len(sys.argv) > 1 else 'data/shaders/ryujinx'
    n, size = pack(folder)
    print(f'{n} shaders packed into {pathlib.Path(folder) / "shaders.pack"} ({size / 2**20:.0f} MiB)')


if __name__ == '__main__':
    main()
