/* proto.c - the escaping of TERM47's link lines (proto.h). Plain C: the PC tests use it too.
 * Copyright (C) 2026 Victor Valdes - GPL-3.0-or-later. */
#include "proto.h"

static const char HEX[] = "0123456789ABCDEF";

int t47_encode(const uint8_t *in, int n, char *out, int cap, int *outlen) {
  int i = 0, o = 0;
  for (; i < n; i++) {
    uint8_t c = in[i];
    int need = (c >= 0x20 && c < 0x7f && c != '\\') ? 1 : (c == '\\' || c == 0x1b) ? 2 : 3;
    if (o + need > cap) break;
    if (need == 1) out[o++] = (char)c;
    else if (c == '\\') { out[o++] = '\\'; out[o++] = '\\'; }
    else if (c == 0x1b) { out[o++] = '\\'; out[o++] = 'e'; }
    else { out[o++] = '\\'; out[o++] = HEX[c >> 4]; out[o++] = HEX[c & 15]; }
  }
  *outlen = o;
  return i;
}

static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

int t47_decode(const char *in, int n, uint8_t *out) {
  int o = 0;
  for (int i = 0; i < n; i++) {
    char c = in[i];
    if (c != '\\') { out[o++] = (uint8_t)c; continue; }
    if (i + 1 >= n) break;                      /* a cut line: the half escape dropped */
    char d = in[++i];
    if (d == '\\') out[o++] = '\\';
    else if (d == 'e') out[o++] = 0x1b;
    else if (i + 1 < n && hexv(d) >= 0 && hexv(in[i + 1]) >= 0) { out[o++] = (uint8_t)(hexv(d) * 16 + hexv(in[i + 1])); i++; }
  }
  return o;
}
