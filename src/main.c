/*
 * TERM47 for the SwissMicros DM42n (DMCP5 native program, .pg5): a text terminal, like an old Unix VT100/VT102
 * (C) 2026 Victor Valdes (valdes.vj@gmail.com) - GPL-3.0-or-later
 *
 * Built with the SwissMicros DMCP5 SDK (BSD-3, NOMAS).
 *
 * No calculation: the screen shows what the PC's shell sends, the keys go to it. The link is DMCP5 3.58's serial
 * console over the USB cable; on the PC tools/term47host.py runs the shell (bash, ssh, lynx, w3m ...) in a
 * pseudo-terminal of the calculator's size.
 *
 * Screen (400x240):  rows 0..207   the terminal: 80x26, 66x26 (default), 50x16 or 40x10 characters (SET)
 *                    bottom 32     the soft keys (F1..F6): the commands, from /TERM47/MENU.TXT; >> the next row
 */
#include <main.h>
#include <dmcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vt.h"
#include "link.h"
#include "fonts.h"

#define DIR      "/TERM47"
#define CFG_FILE DIR "/TERM47.CFG"
#define MENU_FILE DIR "/MENU.TXT"
#define TERM_H   (LCD_Y - LCD_MENU_LINES)          /* 208 rows of pixels for the terminal */

/* ------------------------------------------------------------------ */
/*  Settings                                                          */
/* ------------------------------------------------------------------ */
typedef struct { const char *name; int font, cols, rows, x0, y0; } Size;
static const Size SIZES[4] = {
  { "TINY   80x26", 0, 80, 26, 0, 0 },
  { "SMALL  66x26", 1, 66, 26, 2, 0 },
  { "MEDIUM 50x16", 2, 50, 16, 0, 0 },
  { "LARGE  40x10", 3, 40, 10, 0, 4 },
};
static const char *TERMS[] = { "vt102", "vt100", "vt220", "ansi", "xterm-mono" };
#define TERM_CNT 5
static const char *BELLS[] = { "beep", "flash", "off" };

typedef struct { int size, term, crlf, echo, bell, mirror, link, alpha; } Cfg;
static Cfg cfg = { 1, 0, 0, 0, 0, 0, 1, 0 };

/* ------------------------------------------------------------------ */
/*  State                                                             */
/* ------------------------------------------------------------------ */
static Vt *vt;
enum { V_TERM, V_SET, V_INFO };
static int view = V_TERM;
static int shift, ctrl_next;           /* SHIFT pressed; CTRL: the next key as a control character */
static int back;                       /* lines scrolled back (0: the live screen) */
static int setsel;                     /* SET: the line chosen */
static int full;                       /* the whole screen to draw */
static int cur_x = -1, cur_y = -1;     /* where the cursor was drawn */
static char tag[48];                   /* a short message top right */
static uint32_t tag_until;
static int tag_rows;                   /* the terminal rows the message covers */
static int last_state = -1;
static uint32_t flash_until;

static uint32_t now_ms(void) { return sys_current_ms(); }
static const Size *SZ(void) { return &SIZES[cfg.size]; }
static const Font *FONT(void) { return &FONTS[SZ()->font]; }

/* ------------------------------------------------------------------ */
/*  Menus: /TERM47/MENU.TXT                                           */
/* ------------------------------------------------------------------ */
#define MAXPAGES 16
#define TXT 40
#define MK  0xff                         /* a key code follows: VK_*, or K_CTRL */
#define K_CTRL 0x40
typedef struct { char label[8], slabel[8]; uint8_t text[TXT], stext[TXT]; uint8_t n, sn; } Item;
typedef struct { char name[8]; Item it[5]; } Page;
static Page *pages;
static int npages, page;                 /* npages: the file's pages; one more, TERM, built in */

static const char DEFAULT_MENU[] =
  "# TERM47 menu v2. Soft keys: [PAGE] then up to 5 keys a page, F1..F5 (F6 is >>; SHIFT F6 <<)\n"
  "#   label|text            the text is typed when the key is pressed\n"
  "#   label|text|SLABEL|stext   SHIFT and the key: the second one\n"
  "# In the text: \\r Enter, \\t Tab, \\e Esc, \\\\ \\, \\| |, \\xHH a byte, ^C a control key (\\^ is ^),\n"
  "#   {UP} {DOWN} {LEFT} {RIGHT} {HOME} {END} {PGUP} {PGDN}, {CTRL} the next key as a control key.\n"
  "# A text without \\r waits for you: type the rest, then ENTER. Labels up to 6 characters; a label\n"
  "# starting with [ is written \\[ (a line [NAME] starts a page).\n"
  "[SHELL]\n"
  "ls|ls -la\\r|ls -h|ls -lah\\r\n"
  "cd|cd |cd ~|cd ~\\r\n"
  "cat|cat \n"
  "less|less \n"
  "clear|clear\\r|sudo!!|sudo !!\\r\n"
  "[SYS]\n"
  "top|top\\r|htop|htop\\r\n"
  "df|df -h\\r|du|du -sh * \\| sort -h\\r\n"
  "free|free -h\\r|vmstat|vmstat 1 5\\r\n"
  "uptime|uptime\\r|uname|uname -a\\r\n"
  "w|w\\r|who|who\\r\n"
  "[NET]\n"
  "ping|ping -c 4 \n"
  "ssh|ssh \n"
  "lynx|lynx \n"
  "w3m|w3m \n"
  "ip a|ip -br a\\r|ip r|ip r\\r\n"
  "[GIT]\n"
  "status|git status -sb\\r|branch|git branch -a\\r\n"
  "log|git log --oneline -15\\r|pull|git pull\\r\n"
  "diff|git diff\\r|stat|git diff --stat\\r\n"
  "add|git add -A\\r|stash|git stash\\r\n"
  "commit|git commit -m \"\"{LEFT}|push|git push\\r\n"
  "[BUILD]\n"
  "make|make\\r|clean|make clean\\r\n"
  "test|make test\\r|-j4|make -j4\\r\n"
  "grep|grep -rn \"\" .{LEFT}{LEFT}{LEFT}|grep-i|grep -rni \"\" .{LEFT}{LEFT}{LEFT}\n"
  "find|find . -name \"\"{LEFT}|which|which \n"
  "vi|vi |nano|nano \n"
  "[DEBUG]\n"
  "dmesg|dmesg \\| tail -20\\r|dmsg-w|sudo dmesg -w\\r\n"
  "jrnl|journalctl -n 30 --no-pager\\r|jrnl-f|journalctl -f\\r\n"
  "gdb|gdb -q |bt|bt\\r\n"
  "strace|strace -f |ltrace|ltrace \n"
  "tail|tail -f |echo$?|echo $?\\r\n"
  "[PROC]\n"
  "ps|ps aux --sort=-%cpu \\| head\\r|tree|ps -ef --forest\\r\n"
  "pgrep|pgrep -a |pidof|pidof \n"
  "kill|kill |kill-9|kill -9 \n"
  "jobs|jobs -l\\r|bg|bg\\r\n"
  "ports|ss -tulpn\\r|lsof|lsof -i\\r\n"
  "[FILES]\n"
  "lt|ls -lt \\| head\\r|tree|tree -L 2\\r\n"
  "file|file |stat|stat \n"
  "hex|hexdump -C |strs|strings \n"
  "chmod|chmod +x |chown|sudo chown \n"
  "wc|wc -l |diff|diff -u \n"
  "[CTRL]\n"
  "^C|^C\n"
  "^D|^D\n"
  "^Z|^Z|fg|fg\\r\n"
  "TAB|\\t\n"
  "CTRL|{CTRL}\n"
  "[EDIT]\n"
  "<-|{LEFT}|HOME|{HOME}\n"
  "->|{RIGHT}|END|{END}\n"
  "PGUP|{PGUP}\n"
  "PGDN|{PGDN}\n"
  "^R|^R|^L|^L\n"
  "[SYM]\n"
  "/|/|\\\\|\\\\\n"
  "\\||\\||&|&\n"
  "~|~|*|*\n"
  ">|>|<|<\n"
  "$|$|'|'\n"
  "[SYM2]\n"
  "=|=|#|#\n"
  "\"|\"|;|;\n"
  ":|:|@|@\n"
  "(|(|)|)\n"
  "\\[|[|]|]\n";

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* one field of a menu line (up to an unescaped '|'), its escapes decoded; returns where the next field starts */
static const char *field(const char *s, uint8_t *out, int cap, int *n, int raw_label) {
  static const struct { const char *t; uint8_t k; } TOK[] = {
    {"{UP}",VK_UP},{"{DOWN}",VK_DOWN},{"{LEFT}",VK_LEFT},{"{RIGHT}",VK_RIGHT},{"{HOME}",VK_HOME},{"{END}",VK_END},
    {"{PGUP}",VK_PGUP},{"{PGDN}",VK_PGDN},{"{CTRL}",K_CTRL} };
  int o = 0;
  while (*s && *s != '\n' && *s != '|') {
    if (*s == '\\' && s[1] && s[1] != '\n') {
      char c = s[1];
      int b = -1;
      s += 2;
      if (raw_label) b = c;
      else if (c == 'r') b = '\r';
      else if (c == 'n') b = '\n';
      else if (c == 't') b = '\t';
      else if (c == 'e') b = 0x1b;
      else if (c == 'x' && hexv(s[0]) >= 0 && hexv(s[1]) >= 0) { b = hexv(s[0]) * 16 + hexv(s[1]); s += 2; }
      else b = c;                                         /* \\ \| \^ and any other: the character */
      if (o < cap) out[o++] = (uint8_t)b;
      continue;
    }
    if (!raw_label && *s == '^' && s[1] && s[1] != '\n' && s[1] != '|') {
      char c = s[1];
      if (c >= 'a' && c <= 'z') c = (char)(c - 32);
      if (o < cap) out[o++] = (uint8_t)(c == '?' ? 0x7f : (c & 0x1f));
      s += 2;
      continue;
    }
    if (!raw_label && *s == '{') {
      int hit = 0;
      for (unsigned i = 0; i < sizeof TOK / sizeof *TOK; i++) {
        size_t l = strlen(TOK[i].t);
        if (!strncmp(s, TOK[i].t, l)) { if (o + 1 < cap) { out[o++] = MK; out[o++] = TOK[i].k; } s += l; hit = 1; break; }
      }
      if (hit) continue;
    }
    if (o < cap) out[o++] = (uint8_t)*s;
    s++;
  }
  *n = o;
  return *s == '|' ? s + 1 : s;
}

static void parse_menu(const char *s) {
  npages = 0;
  Page *p = NULL;
  int ni = 0;
  while (*s) {
    const char *e = strchr(s, '\n');
    if (!e) e = s + strlen(s);
    if (*s == '#' || *s == '\r' || s == e) { /* a comment, an empty line */ }
    else if (*s == '[' && !memchr(s, '|', (size_t)(e - s))) {     /* [PAGE] */
      if (npages < MAXPAGES) {
        p = &pages[npages++];
        memset(p, 0, sizeof *p);
        int i = 0;
        for (const char *q = s + 1; q < e && *q != ']' && i < 6; q++) p->name[i++] = *q;
        ni = 0;
      } else p = NULL;
    } else if (p && ni < 5) {
      Item *it = &p->it[ni++];
      uint8_t lab[16];
      int n;
      const char *q = field(s, lab, 6, &n, 1);
      memcpy(it->label, lab, (size_t)n); it->label[n] = 0;
      int m;
      q = field(q, it->text, TXT, &m, 0); it->n = (uint8_t)m;
      if (*q && *q != '\n' && *q != '\r') {
        q = field(q, lab, 6, &n, 1);
        memcpy(it->slabel, lab, (size_t)n); it->slabel[n] = 0;
        field(q, it->stext, TXT, &m, 0); it->sn = (uint8_t)m;
      }
    }
    s = *e ? e + 1 : e;
  }
}

/* ------------------------------------------------------------------ */
/*  Files                                                             */
/* ------------------------------------------------------------------ */
static int write_file(const char *path, const char *data) {
  if (!sys_disk_ok()) return 0;
  sys_disk_write_enable(1);
  check_create_dir(DIR);
  FIL f;
  UINT bw = 0;
  int ok = f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
  if (ok) {
    ok = f_write(&f, data, (UINT)strlen(data), &bw) == FR_OK && bw == strlen(data);
    f_close(&f);
  }
  sys_disk_write_enable(0);
  return ok;
}

static int read_file(const char *path, char *buf, int max) {
  if (!sys_disk_ok() || !file_exists(path)) return 0;
  FIL f;
  UINT br = 0;
  if (f_open(&f, path, FA_READ) != FR_OK) return 0;
  f_read(&f, buf, (UINT)(max - 1), &br);
  f_close(&f);
  buf[br] = 0;
  return (int)br;
}

static void save_cfg(void) {
  char s[128];
  sprintf(s, "size=%d\nterm=%d\ncrlf=%d\necho=%d\nbell=%d\nmirror=%d\nlink=%d\nalpha=%d\n",
          cfg.size, cfg.term, cfg.crlf, cfg.echo, cfg.bell, cfg.mirror, cfg.link, cfg.alpha);
  write_file(CFG_FILE, s);
}

static void load_cfg(void) {
  char s[160];
  if (!read_file(CFG_FILE, s, sizeof s)) return;
  static const struct { const char *k; int *v, max; } KEYS[] = {
    {"size", &cfg.size, 3}, {"term", &cfg.term, TERM_CNT - 1}, {"crlf", &cfg.crlf, 1}, {"echo", &cfg.echo, 1},
    {"bell", &cfg.bell, 2}, {"mirror", &cfg.mirror, 1}, {"link", &cfg.link, 1}, {"alpha", &cfg.alpha, 1} };
  for (char *l = strtok(s, "\r\n"); l; l = strtok(NULL, "\r\n")) {
    char *eq = strchr(l, '=');
    if (!eq) continue;
    *eq = 0;
    for (unsigned i = 0; i < sizeof KEYS / sizeof *KEYS; i++)
      if (!strcmp(l, KEYS[i].k)) { int v = atoi(eq + 1); if (v >= 0 && v <= KEYS[i].max) *KEYS[i].v = v; }
  }
}

static void message(const char *s);

static void load_menu(void) {
  char *buf = malloc(8192);
  int ok = 0;
  if (buf && read_file(MENU_FILE, buf, 8192) > 0) {
    if (!strncmp(buf, "# TERM47 soft keys:", 19) && sys_disk_ok()) {   /* TERM47 0.1's file: the new commands */
      write_file(DIR "/MENU.BAK", buf);                                   /* yours kept, if you had changed it */
      write_file(MENU_FILE, DEFAULT_MENU);
      strcpy(buf, DEFAULT_MENU);
      message("New commands: old MENU.TXT in MENU.BAK");
    }
    char *w = buf;                                        /* a file saved on Windows: its CR characters out */
    for (char *r = buf; *r; r++) if (*r != '\r') *w++ = *r;
    *w = 0;
    parse_menu(buf);
    ok = npages > 0;
  }
  if (!ok) {
    parse_menu(DEFAULT_MENU);
    if (sys_disk_ok() && !file_exists(MENU_FILE)) write_file(MENU_FILE, DEFAULT_MENU);   /* yours to change */
  }
  free(buf);
  if (page > npages) page = 0;
}

/* ------------------------------------------------------------------ */
/*  Drawing: everything with lcd_fill_rect, as NAV47                  */
/* ------------------------------------------------------------------ */
static uint8_t px[LCD_X];

static void emit_ink(int y, int x0, int w, int ink) {    /* the runs of px[] equal to ink, as rectangles */
  int i = 0;
  while (i < w) {
    while (i < w && px[i] != ink) i++;
    int j = i;
    while (j < w && px[j] == ink) j++;
    if (j > i) lcd_fill_rect((uint32_t)(x0 + i), (uint32_t)y, (uint32_t)(j - i), 1, ink);
    i = j;
  }
}

/* text in one of the terminal fonts at a pixel position; inv: white on black */
static void draw_text(const Font *f, int x, int y, const char *s, int inv) {
  int n = (int)strlen(s);
  if (x + n * f->w > LCD_X) n = (LCD_X - x) / f->w;
  if (n <= 0) return;
  int w = n * f->w;
  lcd_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, f->h, inv);
  for (int fy = 0; fy < f->h; fy++) {
    for (int i = 0; i < n; i++) {
      uint16_t b = f->bits[(uint8_t)s[i] * f->h + fy];
      for (int k = 0; k < f->w; k++) px[i * f->w + k] = (uint8_t)(((b >> (15 - k)) & 1) ^ inv);
    }
    emit_ink(y + fy, x, w, !inv);
  }
}

static int cursor_shown(void) { return view == V_TERM && !back && vt->cursor_on; }

/* one terminal row */
static void draw_row(int r) {
  const Size *z = SZ();
  const Font *f = FONT();
  int y0 = z->y0 + r * f->h, w = z->cols * f->w;
  int rev = vt->reverse ^ (now_ms() < flash_until);
  lcd_fill_rect(0, (uint32_t)y0, LCD_X, f->h, rev);        /* the whole width: the edges too (the message box) */
  uint16_t mask = (uint16_t)(0xffff << (16 - f->w));
  for (int fy = 0; fy < f->h; fy++) {
    for (int c = 0; c < z->cols; c++) {
      Cell cl = vt_cell(vt, back, r, c);
      uint16_t b = f->bits[cl.ch * f->h + fy];
      if ((cl.at & AT_BOLD) && f->h > 8) b = (uint16_t)((b | (b >> 1)) & mask);   /* bold: thicker (not in the
                                                  8-row fonts, where 0 and 8 would fill in: shown plain there) */
      if ((cl.at & AT_UNDER) && fy == f->h - 1) b = mask;
      int inv = ((cl.at & AT_INV) ? 1 : 0) ^ rev;
      if (cursor_shown() && r == vt->y && c == vt->x) inv ^= 1;
      uint8_t *p = &px[c * f->w];
      for (int k = 0; k < f->w; k++) p[k] = (uint8_t)(((b >> (15 - k)) & 1) ^ inv);
    }
    emit_ink(y0 + fy, z->x0, w, !rev);
  }
}

static void set_tag(const char *s, uint32_t ms) {
  if (tag[0] && vt) for (int r = 0; r < tag_rows && r < vt->rows; r++) vt->dirty[r] = 1;   /* the old box goes */
  snprintf(tag, sizeof tag, "%.47s", s);
  tag_until = now_ms() + ms;
}

static void draw_tag(void) {
  if (!tag[0]) return;
  const Font *f = &FONTS[1];
  int w = (int)strlen(tag) * f->w + 4;
  int x = LCD_X - w, y = 0;
  lcd_fill_rect((uint32_t)x - 1, (uint32_t)y, (uint32_t)w + 1, f->h + 3, 0);      /* a white edge */
  lcd_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, f->h + 2, 1);
  draw_text(f, x + 2, y + 1, tag, 1);
  tag_rows = (f->h + 3 - SZ()->y0 + FONT()->h - 1) / FONT()->h;
}

static const char *menu_keys[6];
static char mk_buf[6][10];

static void draw_menu(void) {
  if (view == V_SET) {
    static const char *K[6] = { "SIZE", "TERM", "ENTER", "ECHO", "BELL", "EXIT" };
    lcd_draw_menu_keys(K);
    return;
  }
  if (view == V_INFO) {
    static const char *K[6] = { "", "", "", "", "", "EXIT" };
    lcd_draw_menu_keys(K);
    return;
  }
  if (page >= npages) {                                   /* the built-in TERM page */
    static const char *K[5] = { "SET", "INFO", "RESET", "LINK", "MIRROR" };
    for (int i = 0; i < 5; i++) {
      strcpy(mk_buf[i], K[i]);
      if (i == 3 && !cfg.link) strcpy(mk_buf[i], "LINK.");
      if (i == 4 && cfg.mirror) strcpy(mk_buf[i], "MIRR*");
      menu_keys[i] = mk_buf[i];
    }
  } else {
    for (int i = 0; i < 5; i++) {
      const Item *it = &pages[page].it[i];
      menu_keys[i] = shift && it->slabel[0] ? it->slabel : it->label;
    }
  }
  strcpy(mk_buf[5], shift ? "<<" : cfg.alpha ? "a >>" : ">>");
  if (ctrl_next) strcpy(mk_buf[5], "CTRL");
  menu_keys[5] = mk_buf[5];
  lcd_draw_menu_keys(menu_keys);
}

static void show(void) {
  lcd_refresh();
  lcd_refresh_wait();        /* the LCD goes out by DMA: done before the buffer is touched again (no flicker) */
}

/* ------------------------------------------------------------------ */
/*  SET and INFO pages                                                */
/* ------------------------------------------------------------------ */
static const char *state_name(int st) {
  switch (st) {
  case LS_OFF: return "off (LINK on the TERM page)";
  case LS_NOCABLE: return "no USB cable";
  case LS_NOPC: return "cable in, port not open";
  case LS_WAIT: return "waiting for term47host.py";
  case LS_ONLINE: return "online";
  }
  return "";
}

#define SET_ITEMS 7
static void set_line(int i, char *s) {
  switch (i) {
  case 0: sprintf(s, "Size      %s", SZ()->name); break;
  case 1: sprintf(s, "TERM      %s", TERMS[cfg.term]); break;
  case 2: sprintf(s, "ENTER     %s", cfg.crlf ? "CR LF" : "CR"); break;
  case 3: sprintf(s, "Echo      %s", cfg.echo ? "local (the PC does not echo)" : "off (the PC echoes)"); break;
  case 4: sprintf(s, "Bell      %s", BELLS[cfg.bell]); break;
  case 5: sprintf(s, "Mirror    %s", cfg.mirror ? "on: the screen to the PC" : "off"); break;
  case 6: sprintf(s, "Link      %s", cfg.link ? "on: DMCP serial console" : "off"); break;
  }
}

static void draw_page(void) {
  const Font *f = &FONTS[2];
  char s[64];
  lcd_fill_rect(0, 0, LCD_X, TERM_H, 0);
  int y = 2;
  if (view == V_SET) {
    draw_text(f, 0, y, " TERM47 SET                                       ", 1); y += f->h + 4;
    for (int i = 0; i < SET_ITEMS; i++) {
      char l[52];
      set_line(i, s);
      snprintf(l, sizeof l, " %-48.48s", s);
      draw_text(f, 0, y, l, i == setsel); y += f->h + 1;
    }
    y += 4;
    draw_text(f, 0, y, " UP DOWN choose  ENTER change  EXIT back", 0); y += f->h;
    sprintf(s, " Link: %s", state_name(link_state()));
    draw_text(f, 0, y, s, 0); y += f->h;
    draw_text(f, 0, y, " The PC gets the new size at once (SIGWINCH).", 0);
  } else {
    sprintf(s, " TERM47 %s  VT102 terminal for the DM42n      ", PROGRAM_VERSION);
    s[50] = 0;
    draw_text(f, 0, y, s, 1); y += f->h + 3;
    static const char *L[] = {
      "(C) 2026 Victor Valdes - GPL-3.0-or-later",
      "PC: python3 tools/term47host.py   (DMCP 3.58)",
      "Letters on the keys A..O, ALPHA: 7..3 = P..Z",
      "SHIFT ENTER ALPHA  SHIFT+letter capital",
      "ENTER Enter  <- Delete  EXIT Esc  UP DOWN arrows",
      "SHIFT <- Ctrl-C  SHIFT R/S Tab  SHIFT - =",
      "SHIFT UP/DOWN scroll back, EXIT live screen",
      "SHIFT EXIT off  SHIFT 0 setup  F6 >> next row",
    };
    for (unsigned i = 0; i < sizeof L / sizeof *L; i++) { draw_text(f, 8, y, L[i], 0); y += f->h; }
    y += 3;
    sprintf(s, "Link: %s %s", state_name(link_state()), link_host());
    draw_text(f, 8, y, s, 0); y += f->h;
    sprintf(s, "%s %s, %lu bytes in, %d lines back", SZ()->name, TERMS[cfg.term], (unsigned long)vt->bytes, vt->sbn);
    draw_text(f, 8, y, s, 0);
  }
}

/* ------------------------------------------------------------------ */
/*  The screen                                                        */
/* ------------------------------------------------------------------ */
static void redraw(void) {
  if (full) {
    lcd_clear_buf();
    if (view == V_TERM) { vt_dirty_all(vt); }
    else draw_page();
    full = 0;
    cur_x = -1;
  }
  if (view == V_TERM) {
    if (tag[0] && now_ms() >= tag_until) {               /* the message goes: the rows under it again */
      tag[0] = 0;
      for (int r = 0; r < tag_rows && r < vt->rows; r++) vt->dirty[r] = 1;
    }
    if (cursor_shown() && (vt->x != cur_x || vt->y != cur_y)) {
      if (cur_y >= 0 && cur_y < vt->rows) vt->dirty[cur_y] = 1;
      vt->dirty[vt->y] = 1;
    }
    int any = 0;
    for (int r = 0; r < vt->rows; r++)
      if (vt->dirty[r]) { draw_row(r); vt->dirty[r] = 0; any = 1; }
    vt->any_dirty = 0;
    cur_x = vt->x; cur_y = vt->y;
    if (tag[0]) draw_tag();
    (void)any;
  }
  draw_menu();
  show();
}

static void message(const char *s) { set_tag(s, 2500); }

/* lines the terminal prints itself (not from the PC): its state, as a VT100's local messages */
static void local(const char *s) {
  if (vt->alt_on) { message(s); return; }
  if (vt->x) vt_puts(vt, "\r\n");
  vt_puts(vt, "\033[7m"); vt_puts(vt, s); vt_puts(vt, "\033[m\r\n");
}

static void apply_size(void) {
  vt_resize(vt, SZ()->cols, SZ()->rows);
  link_size();
  back = 0;
  full = 1;
}

static int local_st = -1;              /* a state to tell about, once it lasts (not the second at the start) */
static uint32_t local_at;

static void link_changed(void) {
  int st = link_state();
  if (local_st >= 0 && st == local_st && now_ms() - local_at > 1500) {
    static const char *TXT_[] = { "TERM47: link off (TERM page, LINK)", "TERM47: plug in the USB cable",
                                  "TERM47: on the PC run  python3 tools/term47host.py",
                                  "TERM47: waiting for term47host.py" };
    local(TXT_[st]);
    local_st = -1;
  }
  if (st == last_state) return;
  if (st == LS_ONLINE) {
    char s[64];
    snprintf(s, sizeof s, "Connected %.36s", link_host());
    message(s);
    local_st = -1;
  } else if (st < LS_ONLINE) {
    local_st = st;
    local_at = now_ms();
  }
  last_state = st;
}

/* ------------------------------------------------------------------ */
/*  Keys                                                              */
/* ------------------------------------------------------------------ */
static void send(const uint8_t *b, int n) {
  if (!n) return;
  back = 0;
  if (cfg.echo) vt_feed(vt, b, n);
  if (link_state() == LS_ONLINE) { link_keys(b, n); return; }
  static uint32_t told;
  if (now_ms() - told > 3000) { message(link_state() == LS_OFF ? "Link off: TERM page, LINK" : "No PC: run term47host.py"); told = now_ms(); }
}
static void send_str(const char *s) { send((const uint8_t *)s, (int)strlen(s)); }

static void send_char(int c) {
  if (ctrl_next) {
    ctrl_next = 0;
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c >= '@' && c <= '_') c &= 0x1f;
    else if (c == '?') c = 0x7f;
  }
  uint8_t b = (uint8_t)c;
  send(&b, 1);
}

/* a menu text: the bytes, with the cursor keys as the terminal's mode wants them and {CTRL} as a prefix */
static void send_text(const uint8_t *t, int n) {
  uint8_t buf[TXT * 4];
  int o = 0;
  for (int i = 0; i < n; i++) {
    if (t[i] == MK && i + 1 < n) {
      uint8_t k = t[++i];
      if (k == K_CTRL) { ctrl_next = 1; continue; }
      const char *s = vt_key(vt, k);
      int l = (int)strlen(s);
      if (o + l < (int)sizeof buf) { memcpy(buf + o, s, (size_t)l); o += l; }
    } else if (t[i] == '\r' && cfg.crlf) { buf[o++] = '\r'; buf[o++] = '\n'; }
    else buf[o++] = t[i];
  }
  send(buf, o);
}

/* the keyboard: letters on the top keys always, the digit block digits (NUM) or letters (ALPHA) */
typedef struct { int key; char alpha, num; } KeyCh;
static const KeyCh KEYCH[] = {
  {KEY_SIGMA,'a','a'},{KEY_INV,'b','b'},{KEY_SQRT,'c','c'},{KEY_LOG,'d','d'},{KEY_LN,'e','e'},{KEY_XEQ,'f','f'},
  {KEY_STO,'g','g'},{KEY_RCL,'h','h'},{KEY_RDN,'i','i'},{KEY_SIN,'j','j'},{KEY_COS,'k','k'},{KEY_TAN,'l','l'},
  {KEY_SWAP,'m','m'},{KEY_CHS,'n','n'},{KEY_E,'o','o'},
  {KEY_7,'p','7'},{KEY_8,'q','8'},{KEY_9,'r','9'},{KEY_DIV,'s','/'},
  {KEY_4,'t','4'},{KEY_5,'u','5'},{KEY_6,'v','6'},{KEY_MUL,'w','*'},
  {KEY_1,'x','1'},{KEY_2,'y','2'},{KEY_3,'z','3'},{KEY_SUB,'-','-'},
  {KEY_0,'/','0'},{KEY_DOT,'.','.'},{KEY_RUN,' ',' '},{KEY_ADD,'_','+'},
};

static const KeyCh *keych(int key) {
  for (unsigned i = 0; i < sizeof KEYCH / sizeof *KEYCH; i++) if (KEYCH[i].key == key) return &KEYCH[i];
  return NULL;
}

/* ------------------------------------------------------------------ */
/*  Setup menu (SHIFT 0) with system items                            */
/* ------------------------------------------------------------------ */
#define MI_T47_ABOUT 1
static const uint8_t mid_menu[] = {
  MI_SET_TIME, MI_SET_DATE, MI_BEEP_MUTE, MI_SLOW_AUTOREP, MI_MSC, MI_SYSTEM_ENTER, MI_T47_ABOUT, 0 };
static const smenu_t MID_MENU = { "TERM47 Setup", mid_menu, NULL, NULL };

static int run_menu_item(uint8_t id) {
  if (id == MI_T47_ABOUT) { view = V_INFO; full = 1; return MRET_EXIT; }
  return MRET_UNIMPL;
}
static const char *menu_line_str(uint8_t id, char *s, const int slen) {
  (void)s; (void)slen;
  return id == MI_T47_ABOUT ? "About TERM47 >" : NULL;
}

static void set_change(int i) {
  switch (i) {
  case 0: cfg.size = (cfg.size + 1) % 4; apply_size(); break;
  case 1: cfg.term = (cfg.term + 1) % TERM_CNT; message("The new TERM: in a new session"); break;
  case 2: cfg.crlf ^= 1; break;
  case 3: cfg.echo ^= 1; break;
  case 4: cfg.bell = (cfg.bell + 1) % 3; break;
  case 5: cfg.mirror ^= 1; link_mirror(cfg.mirror); break;
  case 6: cfg.link ^= 1; if (cfg.link) link_on(cfg.mirror); else { link_bye(); link_off(); } break;
  }
  save_cfg();
  full = 1;
}

static void page_key(int key) {
  if (key == KEY_EXIT || key == KEY_F6 || (view == V_INFO && key == KEY_ENTER)) { view = V_TERM; full = 1; return; }
  if (view != V_SET) return;
  if (key == KEY_UP || key == KEY_8) setsel = (setsel + SET_ITEMS - 1) % SET_ITEMS;
  else if (key == KEY_DOWN || key == KEY_2) setsel = (setsel + 1) % SET_ITEMS;
  else if (key == KEY_ENTER || key == KEY_6 || key == KEY_4) set_change(setsel);
  else if (key >= KEY_F1 && key <= KEY_F5) { setsel = key - KEY_F1; set_change(setsel); }
  full = 1;
}

static void term_page(int i) {
  switch (i) {
  case 0: view = V_SET; full = 1; break;
  case 1: view = V_INFO; full = 1; break;
  case 2: vt_reset(vt); back = 0; message("Terminal reset"); full = 1; break;
  case 3: set_change(6); message(cfg.link ? "Link on" : "Link off"); break;
  case 4: set_change(5); message(cfg.mirror ? "Mirror on: dm42view.py" : "Mirror off"); break;
  }
}

static void scroll_back(int n) {
  int b = back + n;
  if (b < 0) b = 0;
  if (b > vt->sbn) b = vt->sbn;
  if (b == back) return;
  back = b;
  vt_dirty_all(vt);
  if (back) { char s[32]; sprintf(s, "BACK %d/%d", back, vt->sbn); set_tag(s, 60000); }
  else tag_until = 0;
}

static void handle_key(int key) {
  if (view != V_TERM) { page_key(key); return; }
  if (key == KEY_SHIFT) { shift = !shift; return; }

  if (shift) {
    shift = 0;
    const KeyCh *k = keych(key);
    switch (key) {
    case KEY_EXIT: SET_ST(STAT_PGM_END); return;                       /* OFF */
    case KEY_ENTER: cfg.alpha ^= 1; message(cfg.alpha ? "ALPHA: letters on 7..3" : "NUM: digits on 7..3"); save_cfg(); return;
    case KEY_0: handle_menu(&MID_MENU, MENU_RESET, 0); full = 1; return;   /* SETUP */
    case KEY_UP: scroll_back(vt->rows - 1); return;
    case KEY_DOWN: scroll_back(-(vt->rows - 1)); return;
    case KEY_BSP: send_str("\003"); return;                             /* Ctrl-C */
    case KEY_RUN: send_str("\t"); return;                               /* Tab */
    case KEY_SUB: send_char('='); return;
    case KEY_DOT: send_char(','); return;
    case KEY_ADD: send_char('|'); return;
    case KEY_F6: page = (page + npages) % (npages + 1); return;          /* << */
    }
    if (key >= KEY_F1 && key <= KEY_F5) {
      if (page >= npages) { term_page(key - KEY_F1); return; }
      const Item *it = &pages[page].it[key - KEY_F1];
      if (it->sn) send_text(it->stext, it->sn); else send_text(it->text, it->n);
      return;
    }
    if (k && k->alpha >= 'a' && k->alpha <= 'z') { send_char(k->alpha - 32); return; }
    return;
  }

  if (back && (key == KEY_UP || key == KEY_DOWN)) { scroll_back(key == KEY_UP ? 1 : -1); return; }
  if (back && key == KEY_EXIT) { scroll_back(-back); return; }

  switch (key) {
  case KEY_ENTER: send_str(cfg.crlf ? "\r\n" : "\r"); return;
  case KEY_BSP: send_char(0x7f); return;
  case KEY_EXIT: send_str("\033"); return;
  case KEY_UP: send_str(vt_key(vt, VK_UP)); return;
  case KEY_DOWN: send_str(vt_key(vt, VK_DOWN)); return;
  case KEY_F6: page = (page + 1) % (npages + 1); return;
  }
  if (key >= KEY_F1 && key <= KEY_F5) {
    if (page >= npages) { term_page(key - KEY_F1); return; }
    const Item *it = &pages[page].it[key - KEY_F1];
    send_text(it->text, it->n);
    return;
  }
  const KeyCh *k = keych(key);
  if (k) send_char(cfg.alpha ? k->alpha : k->num);
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */
/* newlib's abort() needs _exit; the SDK syscalls don't provide one */
void _exit(int code) { (void)code; sys_reset(); for (;;) {} }

static void fatal(const char *msg) {
  lcd_clear_buf();
  draw_text(&FONTS[3], 10, 80, "TERM47", 1);
  draw_text(&FONTS[2], 10, 120, msg, 0);
  draw_text(&FONTS[2], 10, 150, "Load another program from DMCP menu", 0);
  show();
  for (;;) wait_for_key_press();
}

static void program_init(void) {
  run_menu_item_app = run_menu_item;
  menu_line_str_app = menu_line_str;
  switch_usb_powered_freq();
  load_cfg();
  vt = malloc(sizeof(Vt));
  pages = malloc(sizeof(Page) * MAXPAGES);
  if (!vt || !pages || vt_init(vt, SZ()->cols, SZ()->rows, 200)) fatal("Not enough memory");
  load_menu();
  char s[80];
  sprintf(s, "TERM47 %s - a VT102 terminal for the DM42n, %dx%d\r\n", PROGRAM_VERSION, SZ()->cols, SZ()->rows);
  vt_puts(vt, s);
  if (cfg.link) link_on(cfg.mirror);
  full = 1;
}

void program_main(void) {
  uint32_t last_draw = 0;
  program_init();
  link_changed();
  redraw();

  for (;;) {
    uint32_t now = now_ms();
    if (cfg.link && link_service(vt, vt->cols, vt->rows, TERMS[cfg.term], now)) {
      const char *m = link_msg();
      if (m[0]) message(m);
    }
    link_changed();
    if (vt->bell) {
      vt->bell = 0;
      if (cfg.bell == 0 && !ST(STAT_BEEP_MUTE)) { start_buzzer_freq(880000); sys_delay(40); stop_buzzer(); }
      else if (cfg.bell == 1) { flash_until = now_ms() + 120; vt_dirty_all(vt); }
    }
    if (flash_until && now >= flash_until) { flash_until = 0; vt_dirty_all(vt); }
    int busy = cfg.link && link_busy(now);
    if ((vt->any_dirty || full || (tag[0] && now >= tag_until)) && (!busy || now - last_draw >= 60 || !key_empty())) {
      redraw();
      last_draw = now;
      if (busy || cfg.link) reset_auto_off();
    }

    if (key_empty() && !ST(STAT_PGM_END)) {
      int st = cfg.link ? link_state() : LS_OFF;
      uint32_t t = 0;
      if (st >= LS_WAIT) t = busy ? 10 : 40;                /* a line from the PC does not wake the calculator */
      else if (st >= LS_NOCABLE) t = 250;
      if (tag[0] && (!t || t > 250)) t = 250;
      if (flash_until) t = 20;
      if (t) sys_timer_start(0, t);
      CLR_ST(STAT_RUNNING);
      sys_sleep();
    }

    if (ST(STAT_PGM_END) || ST(STAT_SUSPENDED)) {
      if (!ST(STAT_SUSPENDED)) {                           /* going off */
        save_cfg();
        link_bye();
        link_off();
        lcd_set_buf_cleared(0);
        draw_power_off_image(1);
        LCD_power_off(0);
        SET_ST(STAT_SUSPENDED);
        SET_ST(STAT_OFF);
      }
      continue;
    }
    SET_ST(STAT_RUNNING);
    CLR_ST(STAT_SUSPENDED);

    if (ST(STAT_OFF)) {
      LCD_power_on();
      rtc_wakeup_delay();
      CLR_ST(STAT_OFF);
      if (cfg.link) { link_on(cfg.mirror); last_state = -2; }
      full = 1;
    }
    if (ST(STAT_POWER_CHANGE)) {
      switch_usb_powered_freq();
      CLR_ST(STAT_POWER_CHANGE);
    }

    if (!key_empty()) reset_auto_off();
    int key = key_pop();
    if (key == 77) { full = 1; lcd_forced_refresh(); continue; }   /* DMCP's serial console: the PC sent L */
    if (key > 0 && key != KEY_DOUBLE_RELEASE) {
      int was_shift = shift, was_page = page, was_view = view, was_alpha = cfg.alpha, was_ctrl = ctrl_next;
      handle_key(key);
      if (view != was_view || view != V_TERM) full = 1;          /* a page: drawn whole */
      else if (shift != was_shift || page != was_page || cfg.alpha != was_alpha || ctrl_next != was_ctrl)
        vt->any_dirty = 1;                                        /* the soft keys' labels changed */
      if (cfg.link) link_service(vt, vt->cols, vt->rows, TERMS[cfg.term], now_ms());   /* the key out at once */
    }
  }
}
