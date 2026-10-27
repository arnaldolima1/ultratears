"""Write the RomFS overlay a bigger scene render size needs (TOTK_RENDER_RES, src/host/render_res.c).

usage: python tools/make_res_overlay.py <width>x<height>     (run in the game folder; the launcher does when the
                                                              resolution is chosen)
Adapted from MacNativeGames' games/totk/scripts/make_output_res_overlay.py (MIT). The scene's render targets come from
agl::utl::DynamicTextureAllocator, a fixed heap; its size and that of its parent are data:
System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs, "Heaps" -> name -> "Size" in KiB:
  DynamicTextureAllocatorMem  113664 KiB, parent Graphics 201728 KiB.
Both grow by the scene's pixel ratio above 1600x900 with a 1.6 margin; the runtime adds the same amount to the
application pool (render_res.c totk_render_res_extra_bytes: keep the formula in step). Input: data/romfs (the player's
own RomFS). Output: data/romfs-patches/scene-<w>x<h>/<path>: made from the player's files, it stays on this PC.
"""
import math
import pathlib
import struct
import sys

import compression.zstd as zstd

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
from romfs import RomFS  # noqa: E402

HEAPDEF = 'System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs'
DTA_KIB, GRAPHICS_KIB = 113664, 201728
SCENE_W, SCENE_H = 1600, 900


def extra_kib(w, h):
    grow = (w * h / (SCENE_W * SCENE_H) - 1) * 1.6
    return math.ceil(DTA_KIB * (1 + grow) / 1024) * 1024 - DTA_KIB


def main():
    if len(sys.argv) != 2 or 'x' not in sys.argv[1].lower():
        sys.exit(__doc__)
    w, h = (int(v) for v in sys.argv[1].lower().split('x'))
    if not (640 <= w <= 4096 and 360 <= h <= 4096) or w * h <= SCENE_W * SCENE_H:
        sys.exit(f'{w}x{h}: only scenes bigger than {SCENE_W}x{SCENE_H} (up to 4096x4096) need the overlay')
    raw = RomFS(ROOT / 'data/romfs').read(HEAPDEF)
    info = zstd.get_frame_info(raw)
    if info.dictionary_id:
        sys.exit('the HeapDef is compressed with a dictionary: not the 1.4.3 file')
    byml = bytearray(zstd.decompress(raw))

    # BYML v7, little endian ("YB"): hash nodes 0xC1 {u24 key index, u8 type, u32 value}; ints (0xD1) inline.
    assert byml[:2] == b'YB' and struct.unpack_from('<H', byml, 2)[0] == 7, 'not a BYML v7 file'
    key_table, _, root_node = struct.unpack_from('<III', byml, 4)
    n = struct.unpack_from('<I', byml, key_table)[0] >> 8
    offs = struct.unpack_from(f'<{n + 1}I', byml, key_table + 4)
    keys = [byml[key_table + o: byml.index(0, key_table + o)].decode() for o in offs[:n]]

    def field(node, name, want):
        assert byml[node] == 0xc1
        for i in range(struct.unpack_from('<I', byml, node)[0] >> 8):
            e = node + 4 + 8 * i
            if keys[struct.unpack_from('<I', byml, e)[0] & 0xffffff] == name:
                assert byml[e + 3] == want, (name, byml[e + 3])
                return e + 4
        raise KeyError(name)

    def child(node, name):
        return struct.unpack_from('<I', byml, field(node, name, 0xc1))[0]

    heaps = child(root_node, 'Heaps')
    dta_at = field(child(heaps, 'DynamicTextureAllocatorMem'), 'Size', 0xd1)
    gfx_at = field(child(heaps, 'Graphics'), 'Size', 0xd1)
    dta, gfx = (struct.unpack_from('<i', byml, a)[0] for a in (dta_at, gfx_at))
    assert (dta, gfx) == (DTA_KIB, GRAPHICS_KIB), f'heap sizes {dta}, {gfx}: not the 1.4.3 HeapDef'
    extra = extra_kib(w, h)
    base = bytes(byml)
    struct.pack_into('<i', byml, dta_at, dta + extra)
    struct.pack_into('<i', byml, gfx_at, gfx + extra)
    changed = [i for i in range(len(base)) if base[i] != byml[i]]
    assert changed and all(dta_at <= i < dta_at + 4 or gfx_at <= i < gfx_at + 4 for i in changed)
    data = zstd.compress(bytes(byml), level=19)
    assert zstd.decompress(data) == bytes(byml)
    target = ROOT / 'data/romfs-patches' / f'scene-{w}x{h}' / HEAPDEF
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    print(f'{target}: DynamicTextureAllocatorMem {dta} -> {dta + extra} KiB, Graphics {gfx} -> {gfx + extra} KiB')


if __name__ == '__main__':
    main()
