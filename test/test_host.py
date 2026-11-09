#!/usr/bin/env python3
"""test_host.py - tools/term47host.py's escaping against src/proto.c's rules (make test): every byte both ways,
the same text as the calculator writes, never a line longer than DMCP's channel buffer allows."""
import os
import random
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import term47host as H

fails = 0


def check(name, got, want):
    global fails
    if got != want:
        fails += 1
        print('FAIL %s: %r, want %r' % (name, got, want))


check('esc', H.encode(b'\x1b[0m')[0], '\\e[0m')
check('crlf', H.encode(b'a\r\n')[0], 'a\\0D\\0A')
check('backslash', H.encode(b'\\')[0], '\\\\')
check('n tilde', H.encode('ñ'.encode())[0], '\\C3\\B1')
check('no half escape', H.encode(b'ab\r', cap=4), ('ab', 2))
check('decode', H.decode('x\\e[1m\\C3\\B1\\\\'), b'x\x1b[1m\xc3\xb1\\')
allb = bytes(range(256))
t, used = H.encode(allb, cap=10000)
check('all bytes', (used, H.decode(t)), (256, allb))
check('printable', all(0x20 <= ord(c) < 0x7f for c in t), True)
rnd = random.Random(47)
for i in range(300):                                     # the lines a session sends: whole bytes, within the cap
    data = bytes(rnd.randrange(256) for _ in range(rnd.randrange(1, 400)))
    out = bytearray()
    while data:
        t, used = H.encode(data)
        check('line length', len('D ' + t) + 2 <= 128, True)     # 0x12 + text + '\n': 2 lines < 256 bytes
        out += H.decode(t)
        data = data[used:]
    if fails:
        break
print('host escaping: %d failed' % fails)
sys.exit(1 if fails else 0)
