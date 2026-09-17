#!/usr/bin/env bash
# Regenerates assets/font-hd: Doom's STCFN font glyphs from a WAD, AI-upscaled 4x.
#
# Needs realesrgan-ncnn-vulkan (Real-ESRGAN's portable Vulkan build, with its
# models folder), from https://github.com/xinntao/Real-ESRGAN/releases
# (realesrgan-ncnn-vulkan-<date>-ubuntu.zip on Linux, -windows.zip on Windows),
# and Python 3 with Pillow (pip install pillow).
#
#   tools/font_upscale.sh /path/to/doom.wad /path/to/realesrgan-ncnn-vulkan
#
# The route that keeps the letter shapes: nearest-neighbour 2x first, then the
# realesrgan-x4plus-anime model (8x in total), box-filtered down to 4x. Straight
# 4x from the model mushes the small counters (S, B, N) into blobs.
set -euo pipefail
wad=${1:?wad path}; esr=${2:?realesrgan-ncnn-vulkan path}
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
python3 "$here/font_extract.py" "$wad" "$work/src" 4
mkdir -p "$work/pre2" "$work/out"
python3 - "$work" <<'PY'
import sys, glob, os
from PIL import Image
w = sys.argv[1]
for f in glob.glob(os.path.join(w, "src", "*.png")):
    im = Image.open(f); im.resize((im.width * 2, im.height * 2), Image.NEAREST).save(os.path.join(w, "pre2", os.path.basename(f)))
PY
"$esr" -i "$work/pre2" -o "$work/out" -n realesrgan-x4plus-anime -m "$(dirname "$esr")/models" -s 4 -j 1:1:1
dst="$here/../assets/font-hd"
mkdir -p "$dst"
python3 - "$work" "$dst" <<'PY'
import sys, os
from PIL import Image
w, dst = sys.argv[1], sys.argv[2]
pad, F = 4, 4
man = []
for line in open(os.path.join(w, "src", "manifest.txt")):
    code, gw, gh, lx, ty, crc = line.split()
    im = Image.open(os.path.join(w, "out", "%03d.png" % int(code))).convert("RGBA")
    im = im.resize((im.width // 2, im.height // 2), Image.BOX)
    im = im.crop((pad * F, pad * F, pad * F + int(gw) * F, pad * F + int(gh) * F))
    im.save(os.path.join(dst, "%03d.png" % int(code)), optimize=True)
    man.append(line.strip())
open(os.path.join(dst, "manifest.txt"), "w").write("# code width height leftoffset topoffset crc32-of-alpha-mask (of the source glyph in the WAD)\n" + "\n".join(man) + "\n")
print(len(man), "glyphs written to", dst)
PY
