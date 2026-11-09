/* proto.h - TERM47's link to the PC (tools/term47host.py): text lines on channel 2 of DMCP5 3.58's serial console.
 *
 * The console carries lines of printable ASCII (a byte 0x10-0x1a switches channels, '\n' ends a line, DMCP keeps
 * 256 bytes a channel), so the terminal's bytes travel escaped:
 *     a byte 0x20-0x7e but '\' as it is,  '\' as "\\",  ESC as "\e",  any other byte as '\' + two hex digits
 *     (uppercase: "\0D" is CR, "\C3\B1" is n tilde in UTF-8)
 *
 *   calculator -> PC                         PC -> calculator
 *   T47 HELLO <ver> <cols> <rows> <term>     I <text>       the host's name: the link is up (status line)
 *   T47 SIZE <cols> <rows>                   D <escaped>    output for the screen (at most 120 characters)
 *   T47 BYE                                  M <text>       a message for the status line
 *   K <escaped>     keys typed, answers      X              the session ended (the shell exited)
 *   A <n>           D lines read so far, mod 1000: the PC keeps at most 2 D lines unread (DMCP's 256 bytes)
 *
 * GPL-3.0-or-later (TERM47). */
#ifndef TERM47_PROTO_H
#define TERM47_PROTO_H
#include <stdint.h>

#define T47_CH       2          /* the serial console channel */
#define T47_DMAX   120          /* escaped characters in a D or K line */
#define T47_WINDOW   2          /* D lines the PC may send before an A */

/* escapes in[0..n) into out (at most cap characters, never half an escape): the input bytes used */
int t47_encode(const uint8_t *in, int n, char *out, int cap, int *outlen);
/* the bytes of an escaped text (out has room for n bytes: never longer) */
int t47_decode(const char *in, int n, uint8_t *out);
#endif
