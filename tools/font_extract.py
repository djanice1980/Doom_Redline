# Extract Doom's STCFN font glyphs from a WAD as padded RGBA PNGs plus a manifest.
import struct, sys, os, zlib
from PIL import Image
wad = sys.argv[1]; out = sys.argv[2]; pad = int(sys.argv[3]) if len(sys.argv) > 3 else 4
os.makedirs(out, exist_ok=True)
f = open(wad, "rb"); h = f.read(12); n, off = struct.unpack("<ii", h[4:]); f.seek(off)
lumps = []
for i in range(n):
    e = f.read(16); lo, sz = struct.unpack("<ii", e[:8]); lumps.append((e[8:].rstrip(b"\0").decode("latin1"), lo, sz))
def lump(name):
    for nm, lo, sz in lumps:
        if nm == name: f.seek(lo); return f.read(sz)
pal = lump("PLAYPAL")[:768]
man = []
for nm, lo, sz in lumps:
    if not nm.startswith("STCFN") or sz < 8: continue
    f.seek(lo); d = f.read(sz)
    w, hh, lx, ty = struct.unpack("<hhhh", d[:8])
    cols = struct.unpack("<%di" % w, d[8:8 + 4 * w])
    img = Image.new("RGBA", (w + 2 * pad, hh + 2 * pad), (0, 0, 0, 0)); px = img.load()
    mask = bytearray(w * hh)
    for x in range(w):
        p = cols[x]
        while d[p] != 0xFF:
            top, ln = d[p], d[p + 1]; p += 3
            for k in range(ln):
                y = top + k
                if y < hh:
                    ci = d[p]; px[x + pad, y + pad] = (pal[ci * 3], pal[ci * 3 + 1], pal[ci * 3 + 2], 255); mask[y * w + x] = 1
                p += 1
            p += 1
    code = int(nm[5:])
    img.save(os.path.join(out, "%03d.png" % code))
    man.append("%d %d %d %d %d %08x" % (code, w, hh, lx, ty, zlib.crc32(bytes(mask)) & 0xffffffff))
open(os.path.join(out, "manifest.txt"), "w").write("\n".join(man) + "\n")
print(len(man), "glyphs")
