/* sercon_sim.c - the PC simulator's DMCP5 3.58 serial console (SwissMicros' data sheet) on a real pseudo-terminal:
 * T47_PORT names its slave side; tools/term47host.py --port <that> is the PC. As DMCP:
 *   out: a byte 0x10 + channel before each usb_serial_write (at most 255 bytes a call); with SERIO_SEND_LCD on, each
 *        refresh sends the changed LCD rows on channel 0 ("L <yy>" + 400 pixels 7 bits a byte, as Chess 47's sim)
 *   in:  the bytes are split by channel; channel 2's lines wait in a buffer of USIN_CHAN_SIZE (256) bytes and at
 *        most USIN_MAX_READLINES lines: more is lost (and counted: T47_SIM_LOST on exit), as on the calculator,
 *        so the PC's flow control (A lines) is tested for real. "L" on channel 0: every row again and key 77.
 * Without T47_PORT: no cable. GPL-3.0-or-later. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include "dmcp.h"
#include "sercon_ifc.h"

void sim_push_key(int k);

static int en, lcd, fd = -2, ch, lost, sent_ok[LCD_Y];
static uint8_t sent[LCD_Y][50];
static char q[USIN_CHAN_SIZE * 2];          /* channel 2: the whole lines waiting, '\n' each */
static int nq;
static char part[512];                      /* the line being received */
static int npart, cut;
static char c0[64]; static int n0;

static void lost_report(void) { if (lost) fprintf(stderr, "T47_SIM_LOST %d lines\n", lost); }

static void open_port(void) {
  if (fd != -2) return;
  const char *p = getenv("T47_PORT");
  fd = p ? open(p, O_RDWR | O_NOCTTY | O_NONBLOCK) : -1;
  if (fd >= 0) {
    struct termios t;
    tcgetattr(fd, &t); cfmakeraw(&t); tcsetattr(fd, TCSANOW, &t);
    atexit(lost_report);
  }
}

static void take(void) {                    /* what the PC sent, into the channel buffers */
  open_port();
  if (fd < 0) return;
  uint8_t b[512];
  int n;
  while ((n = (int)read(fd, b, sizeof b)) > 0) {
    for (int i = 0; i < n; i++) {
      uint8_t c = b[i];
      if (c >= US_CHAN0_CHAR && c <= US_CHAN0_CHAR + 10) { ch = c - US_CHAN0_CHAR; continue; }
      if (ch == 0) {                         /* DMCP's own: L = the whole screen */
        if (c == '\n') { c0[n0] = 0; if (!strcmp(c0, "L")) { memset(sent_ok, 0, sizeof sent_ok); sim_push_key(77); } n0 = 0; }
        else if (n0 < 63) c0[n0++] = (char)c;
        continue;
      }
      if (ch != 2) continue;
      if (npart < (int)sizeof part) part[npart++] = (char)c; else cut = 1;
      if (c != '\n') continue;
      int lines = 0;
      for (int k = 0; k < nq; k++) lines += q[k] == '\n';
      if (cut || nq + npart > USIN_CHAN_SIZE || lines >= USIN_MAX_READLINES) lost++;   /* DMCP's buffer full */
      else { memcpy(q + nq, part, (size_t)npart); nq += npart; }
      npart = 0; cut = 0;
    }
  }
}

int usb_serial_ioctl(int op, int val) {
  int rd = val == SER_IOCTL_READ;
  switch (op) {
  case SERIO_IS_ENABLED: if (!rd) en = val ? 1 : 0; return en;
  case SERIO_SEND_LCD: if (!rd) lcd = val ? 1 : 0; return lcd;
  case SERIO_IS_ACTIVE: case SERIO_IS_CONNECTED: open_port(); return en && fd >= 0;
  case SERIO_LINE_READY: case SERIO_LINE_LENGTH: {
    if (val != 2 || !en) return 0;
    take();
    char *nl = memchr(q, '\n', (size_t)nq);
    if (!nl) return 0;
    return op == SERIO_LINE_READY ? 1 : (int)(nl - q) + 1;
  }
  }
  return rd ? 0 : (int)SER_IOCTL_INVALID;
}

static void out(int chn, const char *b, int n) {
  open_port();
  if (fd < 0) return;
  uint8_t id = (uint8_t)(US_CHAN0_CHAR + chn);
  if (write(fd, &id, 1) < 0) return;
  while (n > 0) { int w = (int)write(fd, b, (size_t)n); if (w <= 0) { usleep(1000); continue; } b += w; n -= w; }
}

void usb_serial_write(uint8_t chan_nr, const char *buf, int len) {
  if (!en) return;
  if (len > 255) len = 255;
  out(chan_nr, buf, len);
}

int usb_serial_read_line(uint8_t chan_nr, uint8_t *buf, int buflen, uint8_t *is_overflow) {
  if (is_overflow) *is_overflow = 0;
  if (chan_nr != 2 || usb_serial_ioctl(SERIO_LINE_READY, 2) <= 0) return 0;
  int n = (int)((char *)memchr(q, '\n', (size_t)nq) - q) + 1;
  if (buf && n > buflen) return -1;
  if (buf) { memcpy(buf, q, (size_t)n); if (n < buflen) buf[n] = 0; }
  memmove(q, q + n, (size_t)(nq - n));
  nq -= n;
  return buf ? n : 0;
}

void sim_host_L(void) { memset(sent_ok, 0, sizeof sent_ok); }

void sim_lcd_out(int forced) {
  (void)forced;
  if (!en || !lcd) return;
  for (int y = 0; y < LCD_Y; y++) {
    const uint8_t *d = lcd_line_addr(y) + 2;
    if (sent_ok[y] && !memcmp(sent[y], d, 50)) continue;
    memcpy(sent[y], d, 50); sent_ok[y] = 1;
    char line[4 + 58 + 1];
    int n = sprintf(line, "L %02X", LCD_Y - 1 - y);
    for (int j = 0; j < 58; j++) {
      int v = 0;
      for (int k = 0; k < 7; k++) { int i = j * 7 + k; if (i < 400 && (d[i / 8] >> (i % 8) & 1)) v |= 1 << k; }
      line[n++] = (char)(0x80 | v);
    }
    line[n++] = '\n';
    out(0, line, n);
  }
}
