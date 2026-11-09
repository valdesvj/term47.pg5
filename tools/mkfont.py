#!/usr/bin/env python3
"""mkfont.py - makes src/fonts.h, TERM47's four terminal fonts, from the X11 "misc-fixed" bitmap fonts
(public domain; Debian/Ubuntu package xfonts-base, Arch xorg-fonts-misc), ISO8859-1 encoding:

  index 0x01..0x1f  the VT100 line-drawing glyphs (DEC special graphics, ESC ( 0), as the X fonts hold them
  index 0x20..0x7e  ASCII,   0xa0..0xff  Latin-1 (n tilde, accents)

  TINY    5x8  font in a 5x8 cell    80 x 26
  SMALL   5x8  font in a 6x8 cell    66 x 26   (the default: one more pixel between the letters)
  MEDIUM  8x13 font in an 8x13 cell  50 x 16
  LARGE   10x20 font                 40 x 10

M and W of the 5x8 font are drawn 5 pixels wide (the X font's are 4, like H).
The line-drawing glyphs are stretched to the cell's edges, so the lines join from cell to cell.
Each glyph row is stored most significant bit first (bit 15 = the left pixel), 1 = ink.

    python3 tools/mkfont.py [font dir] > src/fonts.h        (needs Pillow)
"""
import gzip
import os
import sys

from PIL import PcfFontFile

FONTS = [  # name, file, cell w, cell h
    ('tiny', '5x8-ISO8859-1.pcf.gz', 5, 8),
    ('small', '5x8-ISO8859-1.pcf.gz', 6, 8),
    ('medium', '8x13-ISO8859-1.pcf.gz', 8, 13),
    ('large', '10x20-ISO8859-1.pcf.gz', 10, 20),
]


# 5x8 draws M and W 4 pixels wide, like H: 5 pixels here, so they read apart on the calculator's small dots
OVERRIDES = {'5x8-ISO8859-1.pcf.gz': {
    'M': ['.....', '#...#', '##.##', '#.#.#', '#.#.#', '#...#', '#...#', '.....'],
    'W': ['.....', '#...#', '#...#', '#.#.#', '#.#.#', '##.##', '#...#', '.....'],
}}


def glyphs(path, cw, ch):
    f = PcfFontFile.PcfFontFile(gzip.open(path), 'iso8859-1')
    ascent = max(-g[1][1] for g in f.glyph if g)            # the tallest glyph's top above the baseline
    fh = ascent + max(g[1][3] for g in f.glyph if g)        # the font's height: ascent + descent
    out = []
    for code in range(256):
        rows = [[0] * cw for _ in range(ch)]
        g = f.glyph[code]
        if g and (0x20 <= code < 0x7f or code >= 0xa0 or 0 < code < 0x20):
            (_, _), (x0, y0, x1, y1), _, im = g
            top = ascent + y0                                   # y0 is the top, negative above the baseline
            for y in range(im.size[1]):
                for x in range(im.size[0]):
                    if im.getpixel((x, y)):
                        cx, cy = x0 + x, top + y
                        if 0 <= cx < cw and 0 <= cy < ch:
                            rows[cy][cx] = 1
        if 0x0b <= code <= 0x19:                                # join the lines across the cell's spare edges
            gw = g[0][0] if g else cw
            for y in range(ch):                                 # a horizontal line to the glyph's right edge
                if gw < cw and rows[y][gw - 1] and rows[y][gw - 2]:
                    for x in range(gw, cw):
                        rows[y][x] = 1
            gh = fh
            for x in range(cw):                                 # a vertical line to the glyph's bottom
                if gh < ch and rows[gh - 1][x] and rows[gh - 2][x]:
                    for y in range(gh, ch):
                        rows[y][x] = 1
        over = OVERRIDES.get(os.path.basename(path), {}).get(chr(code))
        if over:
            rows = [[1 if c == '#' else 0 for c in ln] + [0] * (cw - len(ln)) for ln in over]
        out.append([sum(b << (15 - i) for i, b in enumerate(r)) for r in rows])
    return out


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else '/usr/share/fonts/X11/misc'
    print('/* fonts.h - made by tools/mkfont.py from the X11 misc-fixed fonts (public domain): do not edit.')
    print(' * TERM47\'s terminal fonts, 256 glyphs each: 0x01-0x1f the DEC line drawing, 0x20-0x7e ASCII,')
    print(' * 0xa0-0xff Latin-1. One uint16 per glyph row, bit 15 = the left pixel, 1 = ink. */')
    print('#ifndef TERM47_FONTS_H\n#define TERM47_FONTS_H\n#include <stdint.h>\n')
    print('typedef struct { const char *name; uint8_t w, h; const uint16_t *bits; } Font;\n')
    for name, fn, cw, ch in FONTS:
        g = glyphs(os.path.join(d, fn), cw, ch)
        print('static const uint16_t FONT_%s[256 * %d] = {' % (name.upper(), ch))
        for code, rows in enumerate(g):
            c = chr(code) if 0x20 < code < 0x7f and code != 0x5c else ''
            print('  ' + ','.join('0x%04x' % r for r in rows) + ',' + ('  /* %02x %s */' % (code, c)))
        print('};\n')
    print('static const Font FONTS[4] = {')
    for name, fn, cw, ch in FONTS:
        print('  { "%s", %d, %d, FONT_%s },' % (name.upper(), cw, ch, name.upper()))
    print('};\n#endif')


if __name__ == '__main__':
    main()
