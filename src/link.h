/* link.h - TERM47's line to the PC over DMCP5 3.58's serial console (the protocol in proto.h).
 * GPL-3.0-or-later. */
#ifndef TERM47_LINK_H
#define TERM47_LINK_H
#include <stdint.h>
#include "vt.h"

enum { LS_OFF, LS_NOCABLE, LS_NOPC, LS_WAIT, LS_ONLINE };

void link_on(int mirror);                 /* the console on (DMCP's own setting given back at link_off) */
void link_off(void);
void link_mirror(int on);                 /* DMCP streams the LCD rows on channel 0 (tools/dm42view.py) */
int  link_state(void);
const char *link_host(void);              /* "I <text>": who is at the other end */
/* reads the PC's lines into the terminal, answers, keeps the handshake: 1 if the screen or the state changed */
int  link_service(Vt *vt, int cols, int rows, const char *term, uint32_t now);
int  link_busy(uint32_t now);             /* data came in the last second: poll fast */
void link_keys(const uint8_t *b, int n);  /* bytes typed: to the shell */
void link_size(void);                     /* the size changed: T47 SIZE at the next service */
void link_bye(void);
const char *link_msg(void);               /* "M <text>" from the PC, once ("" if none) */
#endif
