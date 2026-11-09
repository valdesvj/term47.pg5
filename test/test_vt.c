/* test_vt.c - TERM47's terminal (src/vt.c) and link escaping (src/proto.c) on the PC: make test
 * Each case feeds bytes and compares the screen's text (and the cursor, the answers) with what a VT102 shows.
 * GPL-3.0-or-later. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "vt.h"
#include "proto.h"

static int fails, runs;
static Vt V;

static void row(int r, char *s) {
  int n = 0;
  for (int c = 0; c < V.cols; c++) s[n++] = (char)vt_cell(&V, 0, r, c).ch;
  while (n && s[n - 1] == ' ') n--;
  s[n] = 0;
}

static void expect_row(const char *name, int r, const char *want) {
  char s[VT_MAXC + 1];
  row(r, s);
  runs++;
  if (strcmp(s, want)) { fails++; printf("FAIL %s: row %d is \"%s\", want \"%s\"\n", name, r, s, want); }
}
static void expect_xy(const char *name, int x, int y) {
  runs++;
  if (V.x != x || V.y != y) { fails++; printf("FAIL %s: cursor %d,%d want %d,%d\n", name, V.x, V.y, x, y); }
}
static void expect_int(const char *name, int got, int want) {
  runs++;
  if (got != want) { fails++; printf("FAIL %s: %d, want %d\n", name, got, want); }
}
static void expect_str(const char *name, const char *got, const char *want) {
  runs++;
  if (strcmp(got, want)) { fails++; printf("FAIL %s: \"%s\", want \"%s\"\n", name, got, want); }
}
static void fresh(int c, int r) { vt_resize(&V, c, r); vt_reset(&V); V.sbn = 0; }
static void feed(const char *s) { vt_puts(&V, s); }

int main(void) {
  if (vt_init(&V, 66, 26, 200)) { puts("no memory"); return 1; }

  fresh(10, 4);
  feed("hello\r\nworld");
  expect_row("crlf", 0, "hello"); expect_row("crlf", 1, "world"); expect_xy("crlf", 5, 1);

  fresh(10, 4);
  feed("0123456789AB");                                  /* autowrap: the 11th character on the next line */
  expect_row("wrap", 0, "0123456789"); expect_row("wrap", 1, "AB"); expect_xy("wrap", 2, 1);
  fresh(10, 4);
  feed("0123456789\r\n");                                /* exactly full: no empty line (xenl) */
  expect_row("xenl", 1, ""); expect_xy("xenl", 0, 1);

  fresh(10, 3);
  feed("a\r\nb\r\nc\r\nd");                              /* scroll: a goes into the scrollback */
  expect_row("scroll", 0, "b"); expect_row("scroll", 2, "d"); expect_int("scrollback", V.sbn, 1);
  expect_int("scrollback cell", vt_cell(&V, 1, 0, 0).ch, 'a');
  expect_int("scrollback view row 1", vt_cell(&V, 1, 1, 0).ch, 'b');

  fresh(20, 5);
  feed("\033[3;4HX\033[1;1HY\033[5;20HZ");
  expect_row("cup", 2, "   X"); expect_row("cup", 0, "Y"); expect_row("cup", 4, "                   Z");
  feed("\033[H\033[2J");
  expect_row("ed2", 2, ""); expect_xy("ed2 home", 0, 0);

  fresh(20, 3);
  feed("abcdefgh\033[1;4H\033[K");
  expect_row("el0", 0, "abc");
  feed("\r\nabcdefgh\033[2;4H\033[1K");
  expect_row("el1", 1, "    efgh");
  feed("\033[1;1H\033[2@");
  expect_row("ich", 0, "  abc");
  feed("\033[1;1H\033[3P");
  expect_row("dch", 0, "bc");
  feed("\033[1;2H\033[2X");
  expect_row("ech", 0, "b");

  fresh(20, 5);
  feed("1\r\n2\r\n3\r\n4\r\n5");
  feed("\033[2;4r\033[4;1H\n");                          /* LF on the region's last line: 2..4 scroll */
  expect_row("region", 0, "1"); expect_row("region", 1, "3"); expect_row("region", 2, "4");
  expect_row("region", 3, ""); expect_row("region", 4, "5"); expect_int("region no scrollback", V.sbn, 0);
  feed("\033[2;1H\033M");                                /* RI on the top line of the region */
  expect_row("ri", 1, ""); expect_row("ri", 2, "3"); expect_row("ri", 3, "4");
  feed("\033[r");
  fresh(20, 5);
  feed("1\r\n2\r\n3\r\n4\r\n5\033[2;1H\033[L");
  expect_row("il", 1, ""); expect_row("il", 2, "2"); expect_row("il", 4, "4");
  feed("\033[M");
  expect_row("dl", 1, "2"); expect_row("dl", 3, "4"); expect_row("dl", 4, "");

  fresh(20, 3);
  feed("\033(0lqqk\033(B x");                            /* DEC line drawing, then ASCII again */
  expect_int("dec l", vt_cell(&V, 0, 0, 0).ch, 0x0d);
  expect_int("dec q", vt_cell(&V, 0, 0, 1).ch, 0x12);
  expect_int("dec k", vt_cell(&V, 0, 0, 3).ch, 0x0c);
  expect_int("ascii back", vt_cell(&V, 0, 0, 5).ch, 'x');
  feed("\r\n\016q\017q");                                /* SO / SI with G1 = ASCII here: */
  feed("\033)0\016q\017q");
  expect_int("so g1 dec", vt_cell(&V, 0, 1, 2).ch, 0x12);
  expect_int("si g0", vt_cell(&V, 0, 1, 3).ch, 'q');

  fresh(20, 3);
  feed("Espa\xc3\xb1" "a \xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90 \xe2\x82\xac \xf0\x9f\x98\x80!");
  expect_int("utf8 n tilde", vt_cell(&V, 0, 0, 4).ch, 0xf1);
  expect_int("utf8 box", vt_cell(&V, 0, 0, 7).ch, 0x0d);
  expect_int("utf8 box h", vt_cell(&V, 0, 0, 8).ch, 0x12);
  expect_int("utf8 euro", vt_cell(&V, 0, 0, 11).ch, 'E');
  expect_int("utf8 emoji 2 cells", vt_cell(&V, 0, 0, 15).ch, '!');

  fresh(20, 5);
  char r[64]; int n;
  feed("\033[3;7H\033[6n");
  n = vt_reply(&V, r, sizeof r - 1); r[n] = 0;
  expect_str("dsr", r, "\033[3;7R");
  feed("\033[c");
  n = vt_reply(&V, r, sizeof r - 1); r[n] = 0;
  expect_str("da", r, "\033[?6c");

  fresh(20, 4);
  feed("main\033[?1049h\033[Halt");
  expect_row("alt", 0, "alt");
  feed("\033[?1049l");
  expect_row("main back", 0, "main"); expect_xy("1049 cursor", 4, 0);

  fresh(20, 3);
  feed("abc\033[1;1H\033[4hX\033[4l");
  expect_row("irm", 0, "Xabc");
  feed("\r\n\tT\033[3g");
  expect_xy("tab", 9, 1);

  fresh(20, 5);
  feed("\033[2;4r\033[?6h\033[1;1HO\033[?6l\033[r");     /* origin mode: home is the region's top */
  expect_row("decom", 1, "O");

  fresh(20, 3);
  feed("\033[1;4;7mB\033[0mn");
  expect_int("sgr", vt_cell(&V, 0, 0, 0).at, AT_BOLD | AT_UNDER | AT_INV);
  expect_int("sgr0", vt_cell(&V, 0, 0, 1).at, 0);
  feed("\033[38;5;196;1mR\033[38;2;1;2;3mG");            /* 256 and true colour: skipped, bold kept */
  expect_int("sgr 38;5", vt_cell(&V, 0, 0, 2).at, AT_BOLD);

  fresh(20, 3);
  feed("\033]0;title\007ok\033P1;2|stuff\033\\!");       /* OSC and DCS strings: skipped */
  expect_row("osc", 0, "ok!");

  fresh(20, 4);
  feed("a\r\nb\r\nc\r\nprompt$ ");                       /* resize shorter: the bottom (prompt) kept */
  vt_resize(&V, 20, 2);
  expect_row("resize", 1, "prompt$"); expect_row("resize", 0, "c"); expect_xy("resize cursor", 8, 1);
  vt_resize(&V, 20, 4);

  fresh(20, 3);
  expect_str("arrow normal", vt_key(&V, VK_UP), "\033[A");
  feed("\033[?1h");
  expect_str("arrow app", vt_key(&V, VK_UP), "\033OA");

  fresh(10, 3);
  feed("ab\033[3b");                                     /* REP */
  expect_row("rep", 0, "abbbb");

  /* the link's escaping */
  {
    uint8_t all[256], back[256];
    char enc[1024];
    for (int i = 0; i < 256; i++) all[i] = (uint8_t)i;
    int outlen, used = t47_encode(all, 256, enc, sizeof enc, &outlen);
    expect_int("encode all", used, 256);
    int ok = 1;
    for (int i = 0; i < outlen; i++) if (enc[i] < 0x20 || enc[i] > 0x7e) ok = 0;
    expect_int("encode printable", ok, 1);
    n = t47_decode(enc, outlen, back);
    expect_int("decode len", n, 256);
    expect_int("roundtrip", memcmp(all, back, 256), 0);
    const uint8_t esc[] = "\033[0m";
    used = t47_encode(esc, 4, enc, 3, &outlen);          /* "\\e" fits, "[" fits, "0" not */
    expect_int("encode cap", used, 2); expect_int("encode cap len", outlen, 3);
    enc[outlen] = 0; expect_str("esc as \\e", enc, "\\e[");
    const uint8_t cr[] = "a\r";
    used = t47_encode(cr, 2, enc, 3, &outlen);           /* "\0D" needs 3: never half an escape */
    expect_int("encode no half escape", used, 1);
  }

  printf("%d checks, %d failed\n", runs, fails);
  return fails != 0;
}
