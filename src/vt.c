/* vt.c - TERM47's terminal: VT102 / ANSI escape sequences on a character grid (see vt.h).
 * The sequences follow DEC's VT102 and xterm's ctlseqs; what a one-colour 400 x 240 screen cannot show (colours,
 * blink, double width) is read and ignored, so the programs never get out of step.
 * Copyright (C) 2026 Victor Valdes - GPL-3.0-or-later. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "vt.h"

enum { S_GROUND, S_ESC, S_CSI, S_CHARSET, S_HASH, S_STR, S_STR_ESC, S_SKIP1 };

#define CELL(v, r, c) ((v)->scr[(r) * VT_MAXC + (c)])
static const Cell BLANK = { ' ', 0 };

static int clampi(int a, int lo, int hi) { return a < lo ? lo : a > hi ? hi : a; }

static void mark(Vt *v, int r0, int r1) {
  r0 = clampi(r0, 0, v->rows - 1); r1 = clampi(r1, 0, v->rows - 1);
  for (int r = r0; r <= r1; r++) v->dirty[r] = 1;
  v->any_dirty = 1;
}
void vt_dirty_all(Vt *v) { mark(v, 0, v->rows - 1); }

static void clear_cells(Vt *v, int r, int c0, int c1) {     /* columns c0..c1 inclusive of row r */
  for (int c = c0; c <= c1; c++) CELL(v, r, c) = BLANK;
  mark(v, r, r);
}

static void reply(Vt *v, const char *s) {
  int n = (int)strlen(s);
  if (v->nreply + n <= (int)sizeof v->reply) { memcpy(v->reply + v->nreply, s, (size_t)n); v->nreply += n; }
}

int vt_reply(Vt *v, char *buf, int max) {
  int n = v->nreply < max ? v->nreply : max;
  memcpy(buf, v->reply, (size_t)n);
  memmove(v->reply, v->reply + n, (size_t)(v->nreply - n));
  v->nreply -= n;
  return n;
}

/* ------------------------------------------------------------------ */
/*  scrolling                                                         */
/* ------------------------------------------------------------------ */
static void sb_push(Vt *v, int r) {
  if (!v->sb || v->alt_on) return;
  memcpy(&v->sb[v->sbhead * VT_MAXC], &CELL(v, r, 0), sizeof(Cell) * VT_MAXC);
  v->sbhead = (v->sbhead + 1) % v->sbcap;
  if (v->sbn < v->sbcap) v->sbn++;
}

static void scroll_up(Vt *v, int t, int b, int n) {
  if (n <= 0) return;
  if (n > b - t + 1) n = b - t + 1;
  if (t == 0) for (int i = 0; i < n; i++) sb_push(v, i);
  memmove(&CELL(v, t, 0), &CELL(v, t + n, 0), sizeof(Cell) * VT_MAXC * (size_t)(b - t + 1 - n));
  for (int r = b - n + 1; r <= b; r++) clear_cells(v, r, 0, VT_MAXC - 1);
  mark(v, t, b);
}

static void scroll_down(Vt *v, int t, int b, int n) {
  if (n <= 0) return;
  if (n > b - t + 1) n = b - t + 1;
  memmove(&CELL(v, t + n, 0), &CELL(v, t, 0), sizeof(Cell) * VT_MAXC * (size_t)(b - t + 1 - n));
  for (int r = t; r < t + n; r++) clear_cells(v, r, 0, VT_MAXC - 1);
  mark(v, t, b);
}

static void index_down(Vt *v) {          /* IND / LF */
  if (v->y == v->bot) scroll_up(v, v->top, v->bot, 1);
  else if (v->y < v->rows - 1) v->y++;
}
static void index_up(Vt *v) {            /* RI */
  if (v->y == v->top) scroll_down(v, v->top, v->bot, 1);
  else if (v->y > 0) v->y--;
}

/* ------------------------------------------------------------------ */
/*  reset, size                                                       */
/* ------------------------------------------------------------------ */
static void clear_screen(Cell *s) {
  for (int i = 0; i < VT_MAXR * VT_MAXC; i++) s[i] = BLANK;
}

static void save_cursor(Vt *v, VtSave *s) {
  s->x = v->x; s->y = v->y; s->at = v->at; s->origin = v->origin; s->wrapnext = v->wrapnext;
  s->gl = v->gl; s->g[0] = v->g[0]; s->g[1] = v->g[1];
}
static void restore_cursor(Vt *v, const VtSave *s) {
  v->x = clampi(s->x, 0, v->cols - 1); v->y = clampi(s->y, 0, v->rows - 1);
  v->at = s->at; v->origin = s->origin; v->wrapnext = s->wrapnext;
  v->gl = s->gl; v->g[0] = s->g[0]; v->g[1] = s->g[1];
}

static void reset_tabs(Vt *v) { for (int c = 0; c < VT_MAXC; c++) v->tabs[c] = c && c % 8 == 0; }

void vt_reset(Vt *v) {
  v->scr = v->main; v->alt_on = 0;
  clear_screen(v->main); clear_screen(v->alt);
  v->x = v->y = v->wrapnext = 0;
  v->at = 0; v->top = 0; v->bot = v->rows - 1;
  v->origin = 0; v->autowrap = 1; v->insert = 0; v->lnm = 0;
  v->appcursor = v->appkeypad = 0; v->cursor_on = 1; v->reverse = 0;
  v->g[0] = v->g[1] = 0; v->gl = 0;
  reset_tabs(v);
  save_cursor(v, &v->saved); save_cursor(v, &v->saved_alt);
  v->state = S_GROUND; v->need = 0; v->lastg = ' ';
  vt_dirty_all(v);
}

int vt_init(Vt *v, int cols, int rows, int sb_lines) {
  memset(v, 0, sizeof *v);
  v->main = malloc(sizeof(Cell) * VT_MAXR * VT_MAXC);
  v->alt = malloc(sizeof(Cell) * VT_MAXR * VT_MAXC);
  if (!v->main || !v->alt) return -1;
  if (sb_lines > 0 && (v->sb = malloc(sizeof(Cell) * VT_MAXC * (size_t)sb_lines))) v->sbcap = sb_lines;
  v->cols = clampi(cols, 2, VT_MAXC); v->rows = clampi(rows, 2, VT_MAXR);
  vt_reset(v);
  return 0;
}

/* a new size (the SET page): the text stays where it is, cut at the new edges; the bottom lines kept when the
   screen gets shorter (the prompt is there) */
void vt_resize(Vt *v, int cols, int rows) {
  cols = clampi(cols, 2, VT_MAXC); rows = clampi(rows, 2, VT_MAXR);
  if (rows < v->rows) {
    int cut = v->y - (rows - 1);            /* lines above the screen's new end, enough to keep the cursor's */
    if (cut > 0) {
      Cell *keep = v->scr;
      v->scr = v->main;
      if (!v->alt_on) for (int i = 0; i < cut; i++) sb_push(v, i);
      v->scr = keep;
      for (Cell *s = v->main; s; s = s == v->main ? v->alt : NULL) {
        memmove(s, s + cut * VT_MAXC, sizeof(Cell) * VT_MAXC * (size_t)(VT_MAXR - cut));
        for (int i = (VT_MAXR - cut) * VT_MAXC; i < VT_MAXR * VT_MAXC; i++) s[i] = BLANK;
      }
      v->y -= cut;
    }
  }
  for (Cell *s = v->main; s; s = s == v->main ? v->alt : NULL)     /* blank what is outside the new size */
    for (int r = 0; r < VT_MAXR; r++)
      for (int c = 0; c < VT_MAXC; c++)
        if (r >= rows || c >= cols) s[r * VT_MAXC + c] = BLANK;
  v->cols = cols; v->rows = rows;
  v->x = clampi(v->x, 0, cols - 1); v->y = clampi(v->y, 0, rows - 1);
  v->top = 0; v->bot = rows - 1; v->wrapnext = 0;
  vt_dirty_all(v);
}

/* ------------------------------------------------------------------ */
/*  characters                                                        */
/* ------------------------------------------------------------------ */
/* Unicode -> glyph: Latin-1 as it is, the box drawing and block characters as the DEC line drawing (the light
   lines; heavy and double lines alike), a few symbols as their nearest ASCII */
uint8_t vt_glyph(uint32_t cp) {
  if (cp >= 0x20 && cp < 0x7f) return (uint8_t)cp;
  if (cp >= 0xa0 && cp <= 0xff) return (uint8_t)cp;
  if (cp >= 0x2500 && cp <= 0x257f) {               /* box drawing: by which arms the character has */
    static const uint8_t arms[128] = {               /* bits: 1 left, 2 right, 4 up, 8 down */
      /* 2500 */ 3,3,12,12,3,3,12,12,3,3,12,12,10,10,10,10,
      /* 2510 */ 9,9,9,9,6,6,6,6,5,5,5,5,14,14,14,14,
      /* 2520 */ 14,14,14,14,13,13,13,13,13,13,13,13,11,11,11,11,
      /* 2530 */ 11,11,11,11,7,7,7,7,7,7,7,7,15,15,15,15,
      /* 2540 */ 15,15,15,15,15,15,15,15,15,15,15,15,3,3,12,12,
      /* 2550 */ 3,12,10,10,10,9,9,9,6,6,6,5,5,5,14,14,
      /* 2560 */ 14,13,13,13,11,11,11,7,7,7,15,15,15,10,9,5,
      /* 2570 */ 6,0,0,0,1,4,2,8,1,4,2,8,3,12,3,12,
    };
    switch (arms[cp - 0x2500]) {
    case 3: case 1: case 2: return 0x12;   /* ─ */
    case 12: case 4: case 8: return 0x19;  /* │ */
    case 10: return 0x0d;                  /* ┌ */
    case 9: return 0x0c;                   /* ┐ */
    case 6: return 0x0e;                   /* └ */
    case 5: return 0x0b;                   /* ┘ */
    case 14: return 0x15;                  /* ├ */
    case 13: return 0x16;                  /* ┤ */
    case 11: return 0x18;                  /* ┬ */
    case 7: return 0x17;                   /* ┴ */
    case 15: return 0x0f;                  /* ┼ */
    }
    return '+';
  }
  switch (cp) {
  case 0x2588: case 0x2589: case 0x258a: case 0x258b: case 0x2592: case 0x2593: case 0x2591: return 0x02;  /* ▒ */
  case 0x25c6: case 0x2666: return 0x01;           /* ◆ */
  case 0x2264: return 0x1a; case 0x2265: return 0x1b; case 0x03c0: return 0x1c; case 0x2260: return 0x1d;
  case 0x2022: case 0x2219: case 0x22c5: return 0xb7;          /* bullets: · */
  case 0x2018: case 0x2019: case 0x201a: case 0x2032: return '\'';
  case 0x201c: case 0x201d: case 0x201e: case 0x2033: return '"';
  case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return '-';
  case 0x2026: return 0xb7;                        /* … */
  case 0x2190: return '<'; case 0x2192: return '>'; case 0x2191: return '^'; case 0x2193: return 'v';
  case 0x25b2: case 0x25b4: return '^'; case 0x25bc: case 0x25be: return 'v';
  case 0x25b6: case 0x25b8: case 0x276f: return '>'; case 0x25c0: case 0x25c2: case 0x276e: return '<';
  case 0x20ac: return 'E';                         /* € */
  case 0x2713: case 0x2714: return 'v'; case 0x2717: case 0x2718: return 'x';
  case 0x00a0: return ' ';
  }
  return '?';
}

static void put_glyph(Vt *v, uint8_t g) {
  if (v->wrapnext && v->autowrap) {
    v->x = 0; v->wrapnext = 0;
    index_down(v);
  }
  if (v->insert && v->x < v->cols - 1)
    memmove(&CELL(v, v->y, v->x + 1), &CELL(v, v->y, v->x), sizeof(Cell) * (size_t)(v->cols - 1 - v->x));
  CELL(v, v->y, v->x).ch = g;
  CELL(v, v->y, v->x).at = (uint8_t)v->at;
  mark(v, v->y, v->y);
  v->lastg = g;
  if (v->x == v->cols - 1) v->wrapnext = 1;
  else v->x++;
}

static void print_cp(Vt *v, uint32_t cp) {
  if (cp >= 0x300 && cp <= 0x36f) return;          /* combining accents: not shown on their own */
  if (cp == 0x200b || cp == 0xfeff) return;        /* zero width */
  uint8_t g;
  if (cp >= 0x60 && cp <= 0x7e && v->g[v->gl] == 1) g = (uint8_t)(cp - 0x5f);   /* DEC line drawing */
  else if (cp == 0x5f && v->g[v->gl] == 1) g = ' ';
  else g = vt_glyph(cp);
  put_glyph(v, g);
  if (cp >= 0x1100 && ((cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0xa4cf) || (cp >= 0xac00 && cp <= 0xd7a3) ||
                       (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xfe30 && cp <= 0xfe4f) ||
                       (cp >= 0xff00 && cp <= 0xff60) || (cp >= 0xffe0 && cp <= 0xffe6) ||
                       (cp >= 0x1f300 && cp <= 0x1faff) || (cp >= 0x20000 && cp <= 0x3fffd)))
    put_glyph(v, ' ');                             /* a wide character: two cells, as the host counts them */
}

/* ------------------------------------------------------------------ */
/*  control characters                                                */
/* ------------------------------------------------------------------ */
static void tab(Vt *v, int n) {
  while (n-- > 0) {
    int x = v->x + 1;
    while (x < v->cols - 1 && !v->tabs[x]) x++;
    v->x = x < v->cols ? x : v->cols - 1;
  }
  v->wrapnext = 0;
}
static void backtab(Vt *v, int n) {
  while (n-- > 0 && v->x > 0) {
    int x = v->x - 1;
    while (x > 0 && !v->tabs[x]) x--;
    v->x = x;
  }
  v->wrapnext = 0;
}

static void control(Vt *v, uint8_t c) {
  switch (c) {
  case 0x07: v->bell++; break;
  case 0x08: if (v->x > 0) { if (v->wrapnext) v->wrapnext = 0; else v->x--; } else v->wrapnext = 0; break;
  case 0x09: tab(v, 1); break;
  case 0x0a: case 0x0b: case 0x0c: index_down(v); if (v->lnm) v->x = 0; v->wrapnext = 0; break;
  case 0x0d: v->x = 0; v->wrapnext = 0; break;
  case 0x0e: v->gl = 1; break;
  case 0x0f: v->gl = 0; break;
  case 0x18: case 0x1a: v->state = S_GROUND; break;
  case 0x1b: v->state = S_ESC; v->inter = 0; break;
  }
}

/* ------------------------------------------------------------------ */
/*  CSI                                                               */
/* ------------------------------------------------------------------ */
static int P(Vt *v, int i, int def) { return i < v->npar && v->par[i] > 0 ? v->par[i] : def; }
static int P0(Vt *v, int i) { return i < v->npar ? v->par[i] : 0; }

static void goto_xy(Vt *v, int x, int y) {     /* y relative to the origin (DECOM) */
  if (v->origin) y = clampi(y + v->top, v->top, v->bot);
  v->x = clampi(x, 0, v->cols - 1);
  v->y = clampi(y, 0, v->rows - 1);
  v->wrapnext = 0;
}

/* with a wrap pending (the line just filled), the erase starts after the last character, which stays (as xterm-like
   terminals show top's full-width lines) */
static void erase_display(Vt *v, int how) {
  if (how == 0) {
    clear_cells(v, v->y, v->x + (v->wrapnext ? 1 : 0), VT_MAXC - 1);
    for (int r = v->y + 1; r < v->rows; r++) clear_cells(v, r, 0, VT_MAXC - 1);
  } else if (how == 1) {
    for (int r = 0; r < v->y; r++) clear_cells(v, r, 0, VT_MAXC - 1);
    clear_cells(v, v->y, 0, v->x);
  } else {
    for (int r = 0; r < v->rows; r++) clear_cells(v, r, 0, VT_MAXC - 1);
    if (how == 3) v->sbn = 0;
  }
  v->wrapnext = 0;
}

static void erase_line(Vt *v, int how) {
  if (how == 0) clear_cells(v, v->y, v->x + (v->wrapnext ? 1 : 0), VT_MAXC - 1);
  else if (how == 1) clear_cells(v, v->y, 0, v->x);
  else clear_cells(v, v->y, 0, VT_MAXC - 1);
  v->wrapnext = 0;
}

static void use_alt(Vt *v, int on, int clear) {
  if (on == v->alt_on) { if (on && clear) erase_display(v, 2); return; }
  v->alt_on = on;
  v->scr = on ? v->alt : v->main;
  if (on && clear) for (int r = 0; r < v->rows; r++) clear_cells(v, r, 0, VT_MAXC - 1);
  vt_dirty_all(v);
}

static void set_mode(Vt *v, int on) {
  for (int i = 0; i < (v->npar ? v->npar : 1); i++) {
    int m = P0(v, i);
    if (v->priv == '?') {
      switch (m) {
      case 1: v->appcursor = on; break;
      case 3: erase_display(v, 2); goto_xy(v, 0, 0); v->top = 0; v->bot = v->rows - 1; break;  /* 80/132: cleared */
      case 5: if (v->reverse != on) { v->reverse = on; vt_dirty_all(v); } break;
      case 6: v->origin = on; goto_xy(v, 0, 0); break;
      case 7: v->autowrap = on; if (!on) v->wrapnext = 0; break;
      case 25: if (v->cursor_on != on) { v->cursor_on = on; mark(v, v->y, v->y); } break;
      case 47: case 1047: use_alt(v, on, m == 1047 && !on); break;
      case 1048: if (on) save_cursor(v, &v->saved); else restore_cursor(v, &v->saved); break;
      case 1049:
        if (on) { save_cursor(v, &v->saved_alt); use_alt(v, 1, 1); }
        else { use_alt(v, 0, 0); restore_cursor(v, &v->saved_alt); }
        break;
      }
    } else if (!v->priv) {
      if (m == 4) v->insert = on;
      else if (m == 20) v->lnm = on;
    }
  }
}

static void sgr(Vt *v) {
  if (!v->npar) { v->at = 0; return; }
  for (int i = 0; i < v->npar; i++) {
    int a = v->par[i];
    switch (a) {
    case 0: v->at = 0; break;
    case 1: v->at |= AT_BOLD; break;
    case 4: v->at |= AT_UNDER; break;
    case 5: case 6: v->at |= AT_BOLD; break;     /* blink: shown bold */
    case 7: v->at |= AT_INV; break;
    case 21: case 22: v->at &= ~AT_BOLD; break;
    case 24: v->at &= ~AT_UNDER; break;
    case 25: v->at &= ~AT_BOLD; break;
    case 27: v->at &= ~AT_INV; break;
    case 38: case 48: case 58:                    /* 38;5;n and 38;2;r;g;b: the colour's numbers skipped */
      if (i + 1 < v->npar && v->par[i + 1] == 5) i += 2;
      else if (i + 1 < v->npar && v->par[i + 1] == 2) i += 4;
      break;
    default: break;                               /* colours: one colour on this screen */
    }
  }
}

static void csi(Vt *v, uint8_t f) {
  char s[40];
  if (v->inter == '!' && f == 'p') { vt_reset(v); return; }      /* DECSTR */
  if (v->inter) return;                                          /* DECSCUSR and the like: nothing to do */
  switch (f) {
  case '@': {                                                    /* ICH */
    int n = clampi(P(v, 0, 1), 1, v->cols - v->x);
    memmove(&CELL(v, v->y, v->x + n), &CELL(v, v->y, v->x), sizeof(Cell) * (size_t)(v->cols - v->x - n));
    clear_cells(v, v->y, v->x, v->x + n - 1);
    v->wrapnext = 0;
    break;
  }
  case 'A': v->y = clampi(v->y - P(v, 0, 1), v->y >= v->top ? v->top : 0, v->rows - 1); v->wrapnext = 0; break;
  case 'B': case 'e': v->y = clampi(v->y + P(v, 0, 1), 0, v->y <= v->bot ? v->bot : v->rows - 1); v->wrapnext = 0; break;
  case 'C': case 'a': v->x = clampi(v->x + P(v, 0, 1), 0, v->cols - 1); v->wrapnext = 0; break;
  case 'D': v->x = clampi(v->x - P(v, 0, 1), 0, v->cols - 1); v->wrapnext = 0; break;
  case 'E': v->y = clampi(v->y + P(v, 0, 1), 0, v->y <= v->bot ? v->bot : v->rows - 1); v->x = 0; v->wrapnext = 0; break;
  case 'F': v->y = clampi(v->y - P(v, 0, 1), v->y >= v->top ? v->top : 0, v->rows - 1); v->x = 0; v->wrapnext = 0; break;
  case 'G': case '`': v->x = clampi(P(v, 0, 1) - 1, 0, v->cols - 1); v->wrapnext = 0; break;
  case 'H': case 'f': goto_xy(v, P(v, 1, 1) - 1, P(v, 0, 1) - 1); break;
  case 'I': tab(v, P(v, 0, 1)); break;
  case 'J': erase_display(v, P0(v, 0)); break;
  case 'K': erase_line(v, P0(v, 0)); break;
  case 'L': if (v->y >= v->top && v->y <= v->bot) { scroll_down(v, v->y, v->bot, P(v, 0, 1)); v->x = 0; v->wrapnext = 0; } break;
  case 'M':                                                      /* DL: never into the scrollback */
    if (v->y >= v->top && v->y <= v->bot) {
      int n = clampi(P(v, 0, 1), 1, v->bot - v->y + 1);
      memmove(&CELL(v, v->y, 0), &CELL(v, v->y + n, 0), sizeof(Cell) * VT_MAXC * (size_t)(v->bot - v->y + 1 - n));
      for (int r = v->bot - n + 1; r <= v->bot; r++) clear_cells(v, r, 0, VT_MAXC - 1);
      mark(v, v->y, v->bot); v->x = 0; v->wrapnext = 0;
    }
    break;
  case 'P': {                                                    /* DCH */
    int n = clampi(P(v, 0, 1), 1, v->cols - v->x);
    memmove(&CELL(v, v->y, v->x), &CELL(v, v->y, v->x + n), sizeof(Cell) * (size_t)(v->cols - v->x - n));
    clear_cells(v, v->y, v->cols - n, VT_MAXC - 1);
    v->wrapnext = 0;
    break;
  }
  case 'S': if (!v->priv) scroll_up(v, v->top, v->bot, P(v, 0, 1)); break;
  case 'T': if (!v->priv && v->npar <= 1) scroll_down(v, v->top, v->bot, P(v, 0, 1)); break;
  case 'X': clear_cells(v, v->y, v->x, clampi(v->x + P(v, 0, 1) - 1, v->x, v->cols - 1)); v->wrapnext = 0; break;
  case 'Z': backtab(v, P(v, 0, 1)); break;
  case 'b': for (int i = clampi(P(v, 0, 1), 1, 1000); i > 0; i--) put_glyph(v, (uint8_t)v->lastg); break;
  case 'c':
    if (v->priv == '>') reply(v, "\033[>1;95;0c");               /* DA2: a VT220-like terminal */
    else if (!v->priv && P0(v, 0) == 0) reply(v, "\033[?6c");   /* DA: a VT102 */
    break;
  case 'd': goto_xy(v, v->x, P(v, 0, 1) - 1); break;
  case 'g': if (P0(v, 0) == 0) v->tabs[v->x] = 0; else if (P0(v, 0) == 3) memset(v->tabs, 0, sizeof v->tabs); break;
  case 'h': set_mode(v, 1); break;
  case 'l': set_mode(v, 0); break;
  case 'm': if (!v->priv) sgr(v); break;
  case 'n':
    if (P0(v, 0) == 5) reply(v, "\033[0n");
    else if (P0(v, 0) == 6) {
      snprintf(s, sizeof s, "\033[%d;%dR", (v->origin ? v->y - v->top : v->y) + 1, v->x + 1);
      reply(v, s);
    }
    break;
  case 'r':
    if (!v->priv) {
      int t = P(v, 0, 1) - 1, b = P(v, 1, v->rows) - 1;
      b = clampi(b, 0, v->rows - 1); t = clampi(t, 0, v->rows - 1);
      if (t < b) { v->top = t; v->bot = b; goto_xy(v, 0, 0); }
    }
    break;
  case 's': if (!v->priv) save_cursor(v, &v->saved); break;
  case 'u': if (!v->priv) restore_cursor(v, &v->saved); break;
  case 't':                                                      /* window reports: only the size in characters */
    if (P0(v, 0) == 18) { snprintf(s, sizeof s, "\033[8;%d;%dt", v->rows, v->cols); reply(v, s); }
    break;
  case 'x':                                                      /* DECREQTPARM */
    if (P0(v, 0) <= 1) reply(v, P0(v, 0) ? "\033[3;1;1;120;120;1;0x" : "\033[2;1;1;120;120;1;0x");
    break;
  }
}

/* ------------------------------------------------------------------ */
/*  ESC                                                               */
/* ------------------------------------------------------------------ */
static void esc(Vt *v, uint8_t c) {
  v->state = S_GROUND;
  switch (c) {
  case '[': v->state = S_CSI; v->npar = 0; v->have_par = 0; v->priv = 0; v->inter = 0; memset(v->par, 0, sizeof v->par); break;
  case ']': case 'P': case '^': case '_': case 'X': v->state = S_STR; break;   /* OSC DCS PM APC SOS: skipped */
  case '(': case ')': case '*': case '+': v->inter = c; v->state = S_CHARSET; break;
  case '#': v->state = S_HASH; break;
  case ' ': case '%': v->state = S_SKIP1; break;
  case '7': save_cursor(v, &v->saved); break;
  case '8': restore_cursor(v, &v->saved); break;
  case 'D': index_down(v); v->wrapnext = 0; break;
  case 'E': index_down(v); v->x = 0; v->wrapnext = 0; break;
  case 'M': index_up(v); v->wrapnext = 0; break;
  case 'H': v->tabs[v->x] = 1; break;
  case 'c': vt_reset(v); break;
  case '=': v->appkeypad = 1; break;
  case '>': v->appkeypad = 0; break;
  case 'Z': reply(v, "\033[?6c"); break;
  case 'N': case 'O': break;                                     /* single shifts: G2 / G3 are ASCII here */
  case 'n': case 'o': break;
  default: break;
  }
}

/* ------------------------------------------------------------------ */
/*  the byte stream                                                   */
/* ------------------------------------------------------------------ */
static void byte(Vt *v, uint8_t c) {
  /* in an escape sequence the C0 controls still act (VT100), except in the strings */
  if (v->state != S_STR && v->state != S_STR_ESC && v->state != S_GROUND && c < 0x20 && c != 0x1b) {
    if (c == 0x18 || c == 0x1a) { v->state = S_GROUND; return; }
    control(v, c);
    return;
  }
  switch (v->state) {
  case S_GROUND:
    if (v->need) {                                               /* inside a UTF-8 character */
      if ((c & 0xc0) == 0x80) {
        v->cp = (v->cp << 6) | (c & 0x3f);
        if (--v->need == 0) print_cp(v, v->cp);
        return;
      }
      v->need = 0;
      print_cp(v, '?');
    }
    if (c < 0x20) { control(v, c); return; }
    if (c < 0x7f) { print_cp(v, c); return; }
    if (c == 0x7f) return;
    if (c >= 0xc2 && c <= 0xdf) { v->cp = c & 0x1f; v->need = 1; return; }
    if (c >= 0xe0 && c <= 0xef) { v->cp = c & 0x0f; v->need = 2; return; }
    if (c >= 0xf0 && c <= 0xf4) { v->cp = c & 0x07; v->need = 3; return; }
    print_cp(v, '?');                                            /* a byte that cannot start UTF-8 */
    return;
  case S_ESC: esc(v, c); return;
  case S_CSI:
    if (c >= '0' && c <= '9') {
      if (!v->have_par) { v->have_par = 1; if (v->npar == 0) v->npar = 1; }
      if (v->npar <= 16) { int *p = &v->par[v->npar - 1]; if (*p < 10000) *p = *p * 10 + (c - '0'); }
    } else if (c == ';' || c == ':') {
      if (v->npar == 0) v->npar = 1;
      if (v->npar < 16) v->npar++;
      v->have_par = 1;
    } else if (c >= '<' && c <= '?') {
      v->priv = c;
    } else if (c >= 0x20 && c <= 0x2f) {
      v->inter = c;
    } else if (c >= 0x40 && c <= 0x7e) {
      v->state = S_GROUND;
      csi(v, c);
    } else v->state = S_GROUND;
    return;
  case S_CHARSET: {
    v->state = S_GROUND;
    int set = c == '0' ? 1 : 0;
    if (v->inter == '(') v->g[0] = (uint8_t)set;
    else if (v->inter == ')') v->g[1] = (uint8_t)set;
    v->inter = 0;
    return;
  }
  case S_HASH:
    v->state = S_GROUND;
    if (c == '8') {                                              /* DECALN: the screen full of E */
      for (int r = 0; r < v->rows; r++) for (int x = 0; x < v->cols; x++) { CELL(v, r, x).ch = 'E'; CELL(v, r, x).at = 0; }
      vt_dirty_all(v);
    }
    return;
  case S_SKIP1: v->state = S_GROUND; return;
  case S_STR:                                                    /* until BEL or ESC \ */
    if (c == 0x07 || c == 0x18 || c == 0x1a) v->state = S_GROUND;
    else if (c == 0x1b) v->state = S_STR_ESC;
    return;
  case S_STR_ESC:
    v->state = c == '\\' ? S_GROUND : S_STR;
    return;
  }
}

void vt_feed(Vt *v, const uint8_t *p, int n) {
  v->bytes += (uint32_t)n;
  for (int i = 0; i < n; i++) byte(v, p[i]);
}
void vt_puts(Vt *v, const char *s) { vt_feed(v, (const uint8_t *)s, (int)strlen(s)); }

Cell vt_cell(const Vt *v, int view, int r, int c) {
  if (view <= 0) return v->scr[r * VT_MAXC + c];
  int line = r - view;                                           /* < 0: in the scrollback */
  if (line >= 0) return v->main[line * VT_MAXC + c];
  int back = -line;                                              /* 1 = the last line scrolled off */
  if (back > v->sbn) return BLANK;
  int idx = (v->sbhead - back + v->sbcap) % v->sbcap;
  return v->sb[idx * VT_MAXC + c];
}

const char *vt_key(const Vt *v, int k) {
  static const char *normal[VK_CNT] = { "\033[A", "\033[B", "\033[C", "\033[D", "\033[H", "\033[F", "\033[5~", "\033[6~" };
  static const char *app[VK_CNT]    = { "\033OA", "\033OB", "\033OC", "\033OD", "\033OH", "\033OF", "\033[5~", "\033[6~" };
  if (k < 0 || k >= VK_CNT) return "";
  return v->appcursor ? app[k] : normal[k];
}
