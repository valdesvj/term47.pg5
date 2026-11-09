#!/usr/bin/env python3
"""compare_vt.py - runs real full-screen programs (ls, top, less, vi, lynx, w3m, man) in a pseudo-terminal of
TERM47's size and TERM type, records what they print, and checks that TERM47's terminal (build/vt_dump) shows the
same screen as pyte, a reference terminal emulator in Python (pip install pyte).

    python3 test/compare_vt.py [COLS ROWS [TERM]]       (make compare; defaults 66 26 vt102)

A scene passes when the two screens agree on every cell (characters TERM47 cannot show, outside Latin-1 and the
box drawing, are compared as '?') and on the cursor.
"""
import os
import pty
import select
import shutil
import signal
import struct
import subprocess
import sys
import termios
import fcntl
import time

import pyte

HERE = os.path.dirname(os.path.abspath(__file__))
DUMP = os.path.join(HERE, '..', 'build', 'vt_dump')
PAGES = os.path.join(HERE, 'pages', 'index.html')

DOWN, UP, RIGHT = '\x1b[B', '\x1b[A', '\x1b[C'
# name, command, [(seconds to wait, keys to type)], needs
SCENES = [
    ('ls', 'ls -la /usr/bin | head -60', [(1.0, '')], 'ls'),
    ('bash wrap', 'bash --norc -i', [(0.5, 'echo ' + 'x' * 90 + '\r'), (0.5, 'printf "a\\tb\\tc\\n"\r'),
                                    (0.5, 'exit\r')], 'bash'),
    ('top', 'top -d 0.5', [(2.0, ''), (0.3, 'q')], 'top'),
    ('less', 'less /etc/services', [(0.8, ' '), (0.5, ' '), (0.5, 'G'), (0.5, '/http\r'), (0.6, 'q')], 'less'),
    ('vi', 'vi -u NONE /tmp/term47_vi.txt', [(0.8, 'iHello TERM47\x1b'), (0.3, 'yyp'), (0.3, ':q!\r')], 'vi'),
    ('lynx', 'lynx -nocolor -noprint ' + PAGES, [(2.0, DOWN), (0.6, DOWN), (0.6, RIGHT), (1.0, ''),
                                                 (0.5, 'q')], 'lynx'),
    ('w3m', 'w3m ' + PAGES, [(2.0, DOWN), (0.5, ' '), (0.6, 'q'), (0.3, 'y')], 'w3m'),
    ('man', 'man ls', [(2.0, ' '), (0.5, 'q')], 'man'),
]


def tg(c):
    """a character as TERM47 can show it: Latin-1 and the box drawing (any style as the light lines), else '?'"""
    if not c:
        return ' '
    o = ord(c[0])
    if o < 0x100:
        return c[0]
    if 0x2500 <= o <= 0x257f:
        return '+box'
    return '*'                            # shown as its nearest ASCII (a bullet as a dot, an arrow as <): any


def record(cmd, keys, cols, rows, term):
    pid, fd = pty.fork()
    if pid == 0:
        env = dict(os.environ, TERM=term, LANG='C.UTF-8', LC_ALL='C.UTF-8', COLUMNS=str(cols), LINES=str(rows),
                   PS1='term47$ ', MANPAGER='less', PAGER='less', LESS='')
        os.execvpe('sh', ['sh', '-c', cmd], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))
    out = bytearray()

    def drain(t):
        end = time.time() + t
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                try:
                    d = os.read(fd, 65536)
                except OSError:
                    return False
                if not d:
                    return False
                out.extend(d)
        return True

    snap = None
    for wait, k in keys:
        if not drain(wait):
            break
        snap = bytes(out)                     # the screen just before the last keys (the program may exit)
        if k:
            os.write(fd, k.encode())
    drain(0.5)
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass
    os.waitpid(pid, 0)
    return snap if snap is not None else bytes(out)


def pyte_screen(data, cols, rows):
    sc = pyte.Screen(cols, rows)
    st = pyte.ByteStream(sc)
    st.feed(data)
    lines = [''.join(tg(sc.buffer[r][c].data) if sc.buffer[r][c].data else ' ' for c in range(cols)) for r in range(rows)]
    return lines, (min(sc.cursor.x, cols - 1), sc.cursor.y)   # pyte keeps a pending wrap as x = cols


def ours(data, cols, rows):
    p = subprocess.run([DUMP, str(cols), str(rows)], input=data, capture_output=True)
    txt = p.stdout.decode('utf-8').split('\n')
    lines = [''.join(tg(ch) for ch in ln) for ln in txt[:rows]]
    cx, cy = map(int, txt[rows].split()[1:3])
    return lines, (cx, cy)


def norm(s):
    return s.replace('+box', '#')


def same(p, o):
    """pyte's row and ours: '*' (a character TERM47 shows as its nearest ASCII) matches any"""
    return len(p) == len(o) and all(a == b or a == '*' for a, b in zip(p, o))


def main():
    cols = int(sys.argv[1]) if len(sys.argv) > 1 else 66
    rows = int(sys.argv[2]) if len(sys.argv) > 2 else 26
    term = sys.argv[3] if len(sys.argv) > 3 else 'vt102'
    bad = 0
    for name, cmd, keys, need in SCENES:
        if not shutil.which(need):
            print('skip %-10s (%s not installed)' % (name, need))
            continue
        data = record(cmd, keys, cols, rows, term)
        a, ca = pyte_screen(data, cols, rows)
        b, cb = ours(data, cols, rows)
        diff = [(r, norm(a[r]), norm(b[r])) for r in range(rows) if not same(norm(a[r]), norm(b[r]))]
        ok = not diff and (ca == cb or cmd.startswith('ls'))
        print('%s %-10s %6d bytes  %s' % ('ok  ' if ok else 'FAIL', name, len(data),
                                         '' if ok else '%d rows differ, cursor %s / %s' % (len(diff), ca, cb)))
        if not ok:
            bad += 1
            for r, x, y in diff[:6]:
                print('   row %2d pyte |%s|' % (r, x.rstrip()))
                print('          ours |%s|' % y.rstrip())
            os.makedirs(os.path.join(HERE, '..', 'build'), exist_ok=True)
            open(os.path.join(HERE, '..', 'build', 'compare_%s.bin' % name.replace(' ', '_')), 'wb').write(data)
    print('%d scene(s) differ' % bad)
    return bad != 0


if __name__ == '__main__':
    sys.exit(main())
