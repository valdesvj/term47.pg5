#!/usr/bin/env python3
"""term47host.py - the PC side of TERM47: runs your shell in a pseudo-terminal of the DM42n's size and joins it to
the calculator over the USB cable (DMCP5 3.58's serial console, channel 2; the lines of src/proto.h).

    python3 tools/term47host.py                      # the DM42n (found by name), your $SHELL
    python3 tools/term47host.py /dev/ttyACM1         # a given port
    python3 tools/term47host.py --cmd "ssh ship"     # another program instead of the shell (a remote machine)
    python3 tools/term47host.py --cmd "lynx https://lite.duckduckgo.com"
    python3 tools/term47host.py --png /tmp/dm42.png  # with MIRROR on (TERM page): the calculator's screen as a PNG
    python3 tools/term47host.py --log term47.log     # what went each way (checking)

On the calculator: load TERM47.pg5 (it switches the serial console on). The calculator says HELLO with its size
(66 x 26 ...) and TERM type; this program starts the shell with that TERM, LINES and COLUMNS, and gives it the new
size when you change it on the SET page (SIGWINCH). The session stays when the cable is pulled or the calculator
goes off: it is there again when the calculator comes back. When the shell exits, the next key starts a new one.

Flow: DMCP keeps 256 bytes of a channel, so at most 2 lines of output (120 escaped characters each) wait on the
calculator; it counts the lines it reads and sends that count back (A). A lost count costs a second, not data.

On Linux the user needs access to the port: sudo usermod -aG uucp $USER (Arch) or dialout (Debian), then log in again.
Copyright (C) 2026 Victor Valdes - GPL-3.0-or-later.
"""
import argparse
import fcntl
import os
import pty
import select
import shlex
import signal
import socket
import struct
import sys
import termios
import time

CH = 2
DMAX = 120
WINDOW = 2
ACK_TIMEOUT = 1.0
HEX = '0123456789ABCDEF'


def encode(data, cap=DMAX):
    """bytes -> the escaped text of proto.h, at most cap characters, never half an escape: (text, bytes used)"""
    out = []
    n = 0
    used = 0
    for b in data:
        if 0x20 <= b < 0x7f and b != 0x5c:
            s = chr(b)
        elif b == 0x5c:
            s = '\\\\'
        elif b == 0x1b:
            s = '\\e'
        else:
            s = '\\' + HEX[b >> 4] + HEX[b & 15]
        if n + len(s) > cap:
            break
        out.append(s)
        n += len(s)
        used += 1
    return ''.join(out), used


def decode(text):
    """the escaped text -> bytes"""
    out = bytearray()
    i = 0
    while i < len(text):
        c = text[i]
        if c != '\\':
            out.append(ord(c) & 0xff)
            i += 1
            continue
        if i + 1 >= len(text):
            break
        d = text[i + 1]
        if d == '\\':
            out.append(0x5c)
            i += 2
        elif d == 'e':
            out.append(0x1b)
            i += 2
        elif i + 2 < len(text) and all(x in '0123456789ABCDEFabcdef' for x in text[i + 1:i + 3]):
            out.append(int(text[i + 1:i + 3], 16))
            i += 3
        else:
            i += 2
    return bytes(out)


def find():
    import glob
    found = sorted(glob.glob('/dev/serial/by-id/*SwissMicros*') + glob.glob('/dev/serial/by-id/*DM42*'))
    return found[0] if found else '/dev/ttyACM0'


class Session:
    """the shell (or --cmd) in a pseudo-terminal"""

    def __init__(self, argv, cols, rows, term):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.environ.update(TERM=term, COLUMNS=str(cols), LINES=str(rows), TERM47='1')
            try:
                os.execvp(argv[0], argv)
            except OSError as ex:
                print('term47host: cannot run %s: %s' % (argv[0], ex))
                os._exit(127)
        self.resize(cols, rows)
        fl = fcntl.fcntl(self.fd, fcntl.F_GETFL)
        fcntl.fcntl(self.fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)
        self.term = term

    def resize(self, cols, rows):
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))
        self.cols, self.rows = cols, rows

    def redraw(self):
        """full-screen programs draw again: a size change back and forth (SIGWINCH twice)"""
        c, r = self.cols, self.rows
        self.resize(c, r - 1 if r > 2 else r + 1)
        time.sleep(0.05)
        self.resize(c, r)

    def write(self, b):
        try:
            os.write(self.fd, b)
        except OSError:
            pass

    def close(self):
        try:
            os.kill(self.pid, signal.SIGHUP)
        except OSError:
            pass
        try:
            os.close(self.fd)
        except OSError:
            pass
        try:
            os.waitpid(self.pid, os.WNOHANG)
        except OSError:
            pass


class Host:
    def __init__(self, args):
        self.a = args
        self.port = None
        self.sess = None
        self.out = bytearray()            # the shell's output not yet sent
        self.sent = 0                     # D lines sent (mod 1000 as the calculator's count)
        self.acked = None                 # the calculator's count; None: wait for its first A
        self.last = 0.0                   # the last A or D
        self.cols, self.rows, self.term = 66, 26, 'vt102'
        self.here = False                 # the calculator said HELLO
        self.ch = 0
        self.buf = bytearray()
        self.log = open(args.log, 'a') if args.log else None
        self.screen = None
        if args.png:
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            import dm42port
            self.screen = dm42port.Screen(png=args.png, view=False)
        self.argv = shlex.split(args.cmd) if args.cmd else [os.environ.get('SHELL', '/bin/bash'), '-i']
        self.name = '%s@%s' % (os.environ.get('USER', 'user'), socket.gethostname())

    def say(self, s):
        print('term47host: ' + s, flush=True)

    def logl(self, way, s):
        if self.log:
            self.log.write('%s %s %s\n' % (time.strftime('%H:%M:%S'), way, s))
            self.log.flush()

    # ---------------- the port ----------------
    def open_port(self):
        if self.a.fd is not None:
            self.port = self.a.fd
            return
        told = False
        while True:
            path = self.a.port or find()
            try:
                self.port = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
                break
            except OSError as ex:
                if isinstance(ex, PermissionError):
                    raise SystemExit('no access to %s: add yourself to the uucp (Arch) or dialout (Debian) group' % path)
                if not told:
                    self.say('waiting for the DM42n on %s: load TERM47.pg5, the USB cable in (Ctrl+C quits)'
                             % (self.a.port or '/dev/ttyACM*'))
                    told = True
                time.sleep(1)
        t = termios.tcgetattr(self.port)
        t[0] = 0; t[1] = 0; t[3] = 0                       # raw: no echo, no line editing, no translation
        t[2] |= termios.CLOCAL | termios.CREAD
        termios.tcsetattr(self.port, termios.TCSANOW, t)
        self.say('the DM42n is on %s' % path)

    def send(self, line):
        self.logl('>', line)
        data = bytes([0x10 + CH]) + line.encode('ascii') + b'\n'
        while data:
            try:
                n = os.write(self.port, data)
                data = data[n:]
            except BlockingIOError:
                select.select([], [self.port], [], 0.2)

    # ---------------- the calculator's lines ----------------
    def line(self, raw):
        s = raw.decode('ascii', 'replace').rstrip('\r')
        if not s.startswith('K '):
            self.logl('<', s)
        if s.startswith('K '):
            b = decode(s[2:])
            self.logl('<', 'K %r' % b)
            if not self.sess:
                self.start()
            self.sess.write(b)
        elif s.startswith('A '):
            try:
                n = int(s[2:])
            except ValueError:
                return
            if self.acked is None:
                self.sent = n                              # the first count after HELLO: ours starts there
            self.acked = n
            self.last = time.time()
        elif s.startswith('T47 HELLO'):
            p = s.split()
            try:
                self.cols, self.rows = int(p[3]), int(p[4])
                self.term = p[5] if len(p) > 5 else 'vt102'
            except (IndexError, ValueError):
                pass
            self.here = True
            self.acked = None
            self.say('HELLO from TERM47 %s: %dx%d %s' % (p[2] if len(p) > 2 else '?', self.cols, self.rows, self.term))
            self.send('I ' + self.name)
            if not self.sess:
                self.start()
            else:
                if self.sess.term != self.term:
                    self.send('M TERM %s: in a new session (exit)' % self.term)
                self.sess.resize(self.cols, self.rows)
                self.sess.redraw()
        elif s.startswith('T47 SIZE'):
            p = s.split()
            try:
                self.cols, self.rows = int(p[2]), int(p[3])
            except (IndexError, ValueError):
                return
            if self.sess:
                self.sess.resize(self.cols, self.rows)
            self.say('size %dx%d' % (self.cols, self.rows))
        elif s.startswith('T47 BYE'):
            self.here = False
            self.acked = None
            self.say('the calculator left (the session stays)')

    def start(self):
        self.say('starting %s (%dx%d, TERM=%s)' % (' '.join(self.argv), self.cols, self.rows, self.term))
        self.sess = Session(self.argv, self.cols, self.rows, self.term)
        self.out.clear()

    def read_port(self):
        try:
            data = os.read(self.port, 4096)
        except BlockingIOError:
            return True
        except OSError:
            return False
        if not data:
            return False
        for b in data:
            if 0x10 <= b <= 0x1a:                          # a channel id
                self.ch = b - 0x10
                continue
            if b != 0x0a:
                self.buf.append(b)
                continue
            raw = bytes(self.buf)
            self.buf.clear()
            if self.ch == CH:
                self.line(raw)
            elif self.ch == 0 and self.screen:
                self.screen.row(raw)
        return True

    # ---------------- the shell's output ----------------
    def pump(self):
        if not self.here or self.acked is None:
            return
        if (self.sent - self.acked) % 1000 >= WINDOW and time.time() - self.last > ACK_TIMEOUT:
            self.acked = self.sent                         # an A lost: go on
        while self.out and (self.sent - self.acked) % 1000 < WINDOW:
            text, used = encode(self.out)
            del self.out[:used]
            self.send('D ' + text)
            self.sent = (self.sent + 1) % 1000
            self.last = time.time()

    def run(self):
        self.open_port()
        if self.screen:
            try:
                os.write(self.port, b'\x10L\n')
            except OSError:
                pass
        while True:
            rd = [self.port] + ([self.sess.fd] if self.sess else [])
            wait = 0.05 if self.out else 1.0
            r, _, _ = select.select(rd, [], [], wait)
            if self.port in r and not self.read_port():
                self.say('the port closed (cable out, calculator off): waiting')
                self.here = False
                if self.a.fd is not None:
                    return
                try:
                    os.close(self.port)
                except OSError:
                    pass
                time.sleep(1)
                self.open_port()
                continue
            if self.sess and self.sess.fd in r:
                try:
                    d = os.read(self.sess.fd, 8192)
                except BlockingIOError:
                    d = b''
                except OSError:
                    d = None
                if d is None or (d == b'' and self.sess and not self._alive()):
                    self.say('the session ended')
                    self.sess.close()
                    self.sess = None
                    if self.here:
                        self.send('X')
                elif d:
                    self.out.extend(d)
                    if len(self.out) > 1 << 20:            # the calculator is away: keep the last screenfuls
                        del self.out[:-65536]
            self.pump()

    def _alive(self):
        try:
            pid, _ = os.waitpid(self.sess.pid, os.WNOHANG)
            return pid == 0
        except ChildProcessError:
            return False


def main():
    ap = argparse.ArgumentParser(description='The PC side of TERM47 (DM42n VT102 terminal)')
    ap.add_argument('port', nargs='?', help='the serial port (default: the DM42n by name, else /dev/ttyACM0)')
    ap.add_argument('--cmd', help='the program to run (default: $SHELL -i)')
    ap.add_argument('--png', help='MIRROR on: write the calculator screen to this PNG (needs Pillow)')
    ap.add_argument('--log', help='append the lines each way to this file')
    ap.add_argument('--fd', type=int, help=argparse.SUPPRESS)          # an open port (test/run_sim.py)
    a = ap.parse_args()
    try:
        Host(a).run()
    except KeyboardInterrupt:
        print()


if __name__ == '__main__':
    main()
