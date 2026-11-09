/* link.c - TERM47's line to the PC: DMCP5 3.58's serial console (IFC 3.18), channel 2, the lines of proto.h.
 *
 *   in   usb_serial_read_line(2): whole lines; "D" escaped output -> the terminal, "I" the host, "M" a message,
 *        "X" the session ended. Every D line read is counted; the count goes back as "A <n>" so the PC never has
 *        more than T47_WINDOW lines (under DMCP's 256 bytes a channel) waiting.
 *   out  "K" the keys and the terminal's answers, "T47 HELLO/SIZE/BYE"; whole lines, at most 128 bytes a
 *        usb_serial_write (the data sheet: longer calls lose their ends), short lines joined (as Chess 47).
 *   The A count is sent again every half second while the PC may be waiting for it: a lost A only slows down.
 * Copyright (C) 2026 Victor Valdes - GPL-3.0-or-later. */
#include <main.h>
#include <dmcp.h>
#include <string.h>
#include <stdio.h>
#include "link.h"
#include "proto.h"
#include "sercon_ifc.h"

#define CAP 128
#define RD  SER_IOCTL_READ

static int on, was_en = -1, was_lcd = -1, online;
static int need_hello, need_size, acked = -1;
static uint32_t nread, last_hello, last_rx, last_ack;
static char host[40], msg[48];
static char out[1024];
static int nout;
static char line[USIN_CHAN_SIZE + 1];
static uint8_t bytes[USIN_CHAN_SIZE];

static int sw(int op) { return usb_serial_ioctl(op, RD); }
static void put_back(int op, int v) { if (v == 0 || v == 1) usb_serial_ioctl(op, v); }

void link_on(int mirror) {
  if (on) { link_mirror(mirror); return; }
  was_en = sw(SERIO_IS_ENABLED);
  was_lcd = sw(SERIO_SEND_LCD);
  usb_serial_ioctl(SERIO_IS_ENABLED, 1);
  usb_serial_ioctl(SERIO_SEND_LCD, mirror ? 1 : 0);
  on = 1; online = 0; nout = 0; need_hello = 1; host[0] = 0;
  for (int i = 0; i < USIN_MAX_READLINES && usb_serial_ioctl(SERIO_LINE_READY, T47_CH) > 0; i++)
    usb_serial_read_line(T47_CH, NULL, USIN_CHAN_SIZE, NULL);    /* lines from before: not for this session */
}

void link_off(void) {
  if (!on) return;
  put_back(SERIO_SEND_LCD, was_lcd);
  put_back(SERIO_IS_ENABLED, was_en);
  on = online = 0;
}

void link_mirror(int m) { if (on) usb_serial_ioctl(SERIO_SEND_LCD, m ? 1 : 0); }

int link_state(void) {
  if (!on) return LS_OFF;
  if (sw(SERIO_IS_ACTIVE) <= 0) return LS_NOCABLE;
  if (sw(SERIO_IS_CONNECTED) <= 0) return LS_NOPC;
  return online ? LS_ONLINE : LS_WAIT;
}

const char *link_host(void) { return host; }
const char *link_msg(void) { static char m[48]; strcpy(m, msg); msg[0] = 0; return m; }

static void queue(const char *s) {                     /* one whole line, '\n' added */
  int n = (int)strlen(s);
  if (nout + n + 1 > (int)sizeof out) return;          /* the PC does not read: dropped */
  memcpy(out + nout, s, (size_t)n);
  out[nout + n] = '\n';
  nout += n + 1;
}

static void flush(void) {
  if (!on || !nout) return;
  if (sw(SERIO_IS_CONNECTED) <= 0) { nout = 0; return; }
  int done = 0;
  while (done < nout) {
    int n = 0;
    for (;;) {                                         /* whole lines while they fit in one call */
      char *nl = memchr(out + done + n, '\n', (size_t)(nout - done - n));
      if (!nl) break;
      int l = (int)(nl - (out + done + n)) + 1;
      if (n && n + l > CAP) break;
      n += l;
      if (n >= CAP) break;
    }
    if (!n) break;
    usb_serial_write(T47_CH, out + done, n > CAP ? CAP : n);
    done += n;
  }
  memmove(out, out + done, (size_t)(nout - done));
  nout -= done;
}

void link_keys(const uint8_t *b, int n) {
  char l[T47_DMAX + 3];
  while (n > 0) {
    int len, used = t47_encode(b, n, l + 2, T47_DMAX, &len);
    l[0] = 'K'; l[1] = ' '; l[len + 2] = 0;
    queue(l);
    b += used; n -= used;
  }
  flush();
}

void link_size(void) { need_size = 1; }
void link_bye(void) { if (on && online) { queue("T47 BYE"); flush(); } online = 0; }
int link_busy(uint32_t now) { return on && now - last_rx < 1000; }

static void hello(int cols, int rows, const char *term, uint32_t now) {
  char s[64];
  snprintf(s, sizeof s, "T47 HELLO %s %d %d %s", PROGRAM_VERSION, cols, rows, term);
  queue(s);
  last_hello = now;
  need_hello = need_size = 0;
  acked = -1;                                          /* an A follows: the PC starts its count from it */
}

int link_service(Vt *vt, int cols, int rows, const char *term, uint32_t now) {
  static int last_state = -1;
  int changed = 0;
  if (!on) return 0;
  int st = sw(SERIO_IS_ACTIVE) > 0 && sw(SERIO_IS_CONNECTED) > 0;
  if (!st) { if (online || last_state) changed = 1; online = 0; need_hello = 1; last_state = 0; nout = 0; return changed; }
  if (!last_state) changed = 1;
  last_state = 1;

  if (need_hello || (!online && now - last_hello > 2000)) hello(cols, rows, term, now);
  else if (need_size) {
    char s[32];
    snprintf(s, sizeof s, "T47 SIZE %d %d", cols, rows);
    queue(s);
    need_size = 0;
  }

  for (int i = 0; i < USIN_MAX_READLINES && usb_serial_ioctl(SERIO_LINE_READY, T47_CH) > 0; i++) {
    uint8_t ovf = 0;
    int n = usb_serial_read_line(T47_CH, (uint8_t *)line, USIN_CHAN_SIZE, &ovf);
    if (n < 0) { usb_serial_read_line(T47_CH, NULL, USIN_CHAN_SIZE, NULL); continue; }   /* too long: dropped */
    if (n <= 0) break;
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
    line[n] = 0;
    last_rx = now;
    if (line[0] == 'D' && (line[1] == ' ' || !line[1])) {
      int k = t47_decode(line + 2, n > 2 ? n - 2 : 0, bytes);
      vt_feed(vt, bytes, k);
      nread++;
      if (!online) { online = 1; changed = 1; }
    } else if (line[0] == 'I' && line[1] == ' ') {
      snprintf(host, sizeof host, "%.38s", line + 2);
      online = 1; changed = 1;
    } else if (line[0] == 'M' && line[1] == ' ') {
      snprintf(msg, sizeof msg, "%.46s", line + 2);
      changed = 1;
    } else if (line[0] == 'X' && !line[1]) {
      strcpy(msg, "Session ended: a key starts a new one");
      changed = 1;
    }
  }

  char r[64];
  int k;
  while ((k = vt_reply(vt, r, sizeof r)) > 0) link_keys((uint8_t *)r, k);   /* cursor reports and the like */

  int cnt = (int)(nread % 1000);
  if (cnt != acked || (now - last_rx < 2000 && now - last_ack > 500)) {
    char s[16];
    snprintf(s, sizeof s, "A %d", cnt);
    queue(s);
    acked = cnt;
    last_ack = now;
  }
  flush();
  return changed;
}
