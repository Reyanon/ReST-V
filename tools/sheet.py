#!/usr/bin/env python3
"""Combine harness screenshots (PPM) into one PNG, one row per game.
usage: python tools/sheet.py OUT.png DIR game1 game2 ...  (uses <game>_*.ppm in DIR)"""
import glob, os, sys
from PIL import Image
out, d, games = sys.argv[1], sys.argv[2], sys.argv[3:]
rows = []
for g in games:
    fs = sorted(glob.glob(os.path.join(d, g + "_*.ppm")))[-4:]
    rows.append([Image.open(f).convert("RGB") for f in fs])
cw, ch = 352, 224
sheet = Image.new("RGB", (cw * 4, ch * len(rows)), (40, 0, 40))
for r, ims in enumerate(rows):
    for c, im in enumerate(ims):
        sheet.paste(im, (c * cw, r * ch))
sheet = sheet.resize((sheet.width * 2 // 3, sheet.height * 2 // 3))
sheet.save(out)
print(out, sheet.size)
