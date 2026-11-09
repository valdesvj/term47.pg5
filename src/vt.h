/* vt.h - TERM47's terminal: a VT102 / ANSI screen (the part of xterm that bash, less, top, vi, lynx and w3m use),
 * on a grid of up to 80 x 26 characters, with a scrollback for the lines scrolled off the top.
 * Plain C, no DMCP: the same file runs on the calculator and in the PC tests (test/test_vt.c).
 *
 *   vt_init(&vt, cols, rows, sb_lines)   (malloc: the screens and the scrollback)
 *   vt_feed(&vt, bytes, n)               what the host sent: text (UTF-8) and escape sequences
 *   vt_reply(&vt, buf, max)              what the terminal must answer (cursor position, device attributes)
 *   vt_key_*                             the bytes a key sends (the arrows follow the application cursor mode)
 *   vt.dirty[row]                        rows changed since the program drew them
 *
 * A cell holds a glyph index of src/fonts.h (0x01-0x1f the DEC line drawing, 0x20-0x7e ASCII, 0xa0-0xff
 * Latin-1) and the attributes. Colours are not kept: the screen has one.
 * Copyright (C) 2026 Victor Valdes - GPL-3.0-or-later. */
#ifndef TERM47_VT_H
#define TERM47_VT_H
#include <stdint.h>

#define VT_MAXC 80
#define VT_MAXR 26

#define AT_BOLD   1
#define AT_UNDER  2
#define AT_INV    4

typedef struct { uint8_t ch, at; } Cell;

typedef struct {
  int x, y, at, origin, wrapnext, gl;
  uint8_t g[2];
} VtSave;

typedef struct {
  int cols, rows;
  Cell *scr;                 /* the screen shown: main or alt */
  Cell *main, *alt;          /* VT_MAXR x VT_MAXC each */
  int x, y, wrapnext;
  int at;
  int top, bot;              /* the scrolling region, rows inclusive */
  int origin, autowrap, insert, lnm, appcursor, appkeypad, cursor_on, reverse, alt_on;
  uint8_t g[2];              /* G0, G1: 0 ASCII, 1 DEC line drawing */
  int gl;                    /* 0: G0 in use (SI), 1: G1 (SO) */
  uint8_t tabs[VT_MAXC];
  VtSave saved, saved_alt;
  /* the parser */
  int state;
  int par[16], npar, priv, inter, have_par;
  int lastg;                 /* the last glyph printed (REP) */
  uint32_t cp; int need;     /* UTF-8 */
  /* the scrollback: a ring of lines VT_MAXC wide */
  Cell *sb;
  int sbcap, sbn, sbhead;    /* sbhead: where the next line goes */
  /* out */
  char reply[128];
  int nreply;
  int bell;                  /* BEL count since the program looked */
  uint8_t dirty[VT_MAXR];
  int any_dirty;
  uint32_t bytes;            /* all bytes fed */
} Vt;

int  vt_init(Vt *v, int cols, int rows, int sb_lines);   /* 0 ok, -1 no memory */
void vt_resize(Vt *v, int cols, int rows);
void vt_reset(Vt *v);                                    /* RIS: the screen cleared, the modes back */
void vt_feed(Vt *v, const uint8_t *p, int n);
void vt_puts(Vt *v, const char *s);
int  vt_reply(Vt *v, char *buf, int max);
/* a cell to show: line r of the screen when view = 0; scrolled back by view lines otherwise (view <= vt.sbn) */
Cell vt_cell(const Vt *v, int view, int r, int c);
void vt_dirty_all(Vt *v);
/* the bytes for the cursor keys and the like: up, down, right, left, home, end, page up, page down */
enum { VK_UP, VK_DOWN, VK_RIGHT, VK_LEFT, VK_HOME, VK_END, VK_PGUP, VK_PGDN, VK_CNT };
const char *vt_key(const Vt *v, int k);
/* the glyph shown for a Unicode character (the box drawing as the DEC lines, Latin-1, else '?') */
uint8_t vt_glyph(uint32_t cp);

#endif
