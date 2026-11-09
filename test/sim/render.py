#!/usr/bin/env python3
"""Compose a simulator snapshot (name.pbm + name.txt) into a PNG that looks like the DM42n LCD."""
import sys
from PIL import Image, ImageDraw, ImageFont

BG = (223, 230, 214)     # DM42n reflective LCD
INK = (30, 34, 32)
SCALE = 2
MONO = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"


def render(name, out):
    lines = open(name + ".pbm").read().split("\n")
    w, h = map(int, lines[1].split())
    img = Image.new("RGB", (w, h), BG)
    px = img.load()
    for y in range(h):
        row = lines[2 + y]
        for x in range(w):
            if row[x] == "1":
                px[x, y] = INK
    img = img.resize((w * SCALE, h * SCALE), Image.NEAREST)
    d = ImageDraw.Draw(img)
    fonts = {20: ImageFont.truetype(MONO, 15 * SCALE), 24: ImageFont.truetype(MONO, 18 * SCALE)}
    for ln in open(name + ".txt"):
        ln = ln.rstrip("\n")
        if not ln:
            continue
        font, x, y, inv, s = ln.split("\t", 4)
        font, x, y, inv = int(font), int(x), int(y), int(inv)
        col = BG if inv else INK
        d.text((x * SCALE, (y + 1) * SCALE), s, font=fonts[font], fill=col)
    img.save(out)


if __name__ == "__main__":
    render(sys.argv[1], sys.argv[2])
