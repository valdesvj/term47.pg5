/* vt_dump.c - feeds a byte stream (stdin) to TERM47's terminal and prints the screen as UTF-8 text, the cursor last:
 *   vt_dump COLS ROWS < stream          (test/compare_vt.py checks it against pyte, a reference emulator)
 * GPL-3.0-or-later. */
#include <stdio.h>
#include <stdlib.h>
#include "vt.h"

static void put(int g) {               /* a glyph back as UTF-8: the DEC lines as box drawing */
  static const char *dec[32] = { " ", "◆", "▒", "␉", "␌", "␍", "␊", "°", "±", "␤", "␋", "┘", "┐", "┌", "└", "┼",
                                 "⎺", "⎻", "─", "⎼", "⎽", "├", "┤", "┴", "┬", "│", "≤", "≥", "π", "≠", "£", "·" };
  if (g < 32) fputs(dec[g], stdout);
  else if (g < 0x80) putchar(g);
  else { putchar(0xc0 | (g >> 6)); putchar(0x80 | (g & 0x3f)); }
}

int main(int argc, char **argv) {
  static Vt v;
  if (argc < 3 || vt_init(&v, atoi(argv[1]), atoi(argv[2]), 0)) return 2;
  static unsigned char buf[1 << 16];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, stdin)) > 0) vt_feed(&v, buf, (int)n);
  for (int r = 0; r < v.rows; r++) {
    for (int c = 0; c < v.cols; c++) put(vt_cell(&v, 0, r, c).ch);
    putchar('\n');
  }
  printf("cursor %d %d\n", v.x, v.y);
  return 0;
}
