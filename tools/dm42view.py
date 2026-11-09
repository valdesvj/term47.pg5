#!/usr/bin/env python3
"""dm42view.py - the DM42n's screen in your terminal (or as PNG), sent over USB: NAV47 (>> >> USB) with DMCP5 3.58's
serial console (DMCP streams the LCD rows; the default build), or NAV47 / Chess 47 over their own TinyUSB port.

    python3 tools/dm42view.py                     # the DM42n (found by name), Braille dots: 200 x 60 characters
    python3 tools/dm42view.py /dev/ttyACM1        # or a given port
    python3 tools/dm42view.py --zoom 0,0,200,104  # a part of the screen, bigger (x, y, width, height)
    python3 tools/dm42view.py --blocks            # half blocks: 1 character = 1 x 2 pixels (needs 400 columns)
    python3 tools/dm42view.py --png shots/        # also save every screen as a PNG (needs Pillow)
    python3 tools/dm42view.py --once --png .      # one screen, then exit
    python3 tools/dm42view.py --quiet --latest /tmp/dm42.png &  feh -R 0.5 /tmp/dm42.png
                                                  # a live picture instead of text: one PNG, rewritten each screen

    python3 tools/dm42view.py --raw               # also every line received (channel, text, binary bytes): checking
    python3 tools/dm42view.py --keys              # and the PC keyboard types on the DM42n (serial console)
    python3 tools/dm42view.py --send "SHIFT 7 ENTER"   # press these keys (then exit; --keys / --once to stay)
    python3 tools/dm42view.py --c47 "2 ENTER 3 + SIN STO 01" --once --png .   # C47 commands keyed in (c47keys.py)
    python3 tools/dm42view.py acm.out --once --png .   # the simulator's stream (make sim; build/sim ... HOSTL)

The format is found by itself:
  DMCP5 3.58 serial console: channel-0 lines "L <yy>" + 400 pixels 7 bits a byte (SwissMicros' data sheet); the tool
  sends V (who is there) and L (the whole screen) when it opens the port; a frame when the rows stop for --wait s.
  TinyUSB: "NAV47 LCD 400 240" (or "CHESS47 LCD ..."), 240 lines of 100 hex digits (1 = ink), "END".
On Linux the user needs access to the port: sudo usermod -aG uucp $USER (Arch) or dialout (Debian), then log in again.
"""
import argparse
import os
import shutil
import sys
import time

W, H = 400, 240


def open_port(path):
    """The serial port, raw, read and write (opening it raises DTR: DMCP's "connected"); or a file of a stream."""
    if os.path.exists(path) and not path.startswith('/dev/'):
        return open(path, 'rb'), None
    import termios
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
    t = termios.tcgetattr(fd)
    t[0] = 0; t[1] = 0; t[3] = 0                        # raw: no echo, no line editing, no translation
    t[2] |= termios.CLOCAL | termios.CREAD
    t[6][termios.VMIN] = 0; t[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, t)
    return None, fd


class Dmcp:
    """DMCP5 3.58's serial console, channel 0 (SwissMicros' data sheet, section 8): a byte 0x10 + n switches the
    stream to channel n; lines end in \n; a line is an ASCII part, then a binary part of bytes with the top bit set,
    7 bits each, least significant first. LCD rows: "L <yy>" + 400 pixels (50 bytes): yy hex, 239 = the top row;
    pixel x in byte 49 - x/8, bit 7 - x%8; a dark pixel is a 0 bit."""

    def __init__(self, raw=False):
        self.px = [[0] * W for _ in range(H)]
        self.ch = 0
        self.raw = raw
        self.ident = None
        self.rows = 0

    def clear(self):                                    # after L: DMCP sends only the non-blank rows
        self.px = [[0] * W for _ in range(H)]

    def line(self, data):
        text = bytes(b for b in data if b < 0x80)
        binary = [b & 0x7F for b in data if b >= 0x80]
        if self.raw:
            sys.stderr.write('ch%d %r +%d binary\n' % (self.ch, text, len(binary)))
        if data.endswith(b'\x0f'):                       # cut by an overflow: drop
            return False
        if self.ch == 0 and text.startswith(b'L ') and len(binary) >= 58:
            yy = int(text[2:4], 16)
            if not 0 <= yy < H:
                return False
            bits = 0
            for j, v in enumerate(binary[:58]):
                bits |= v << (7 * j)
            row = bytes((bits >> (8 * i)) & 0xFF for i in range(50))
            self.px[H - 1 - yy] = [0 if (row[49 - x // 8] >> (7 - x % 8)) & 1 else 1 for x in range(W)]
            self.rows += 1
            return True
        if self.ch == 0 and text.startswith(b'V|'):
            self.ident = text.decode('ascii', 'replace')
        return False


def frames(path, raw=False, wait=0.08, opened=None, typing=None):
    """The frames from the serial port (or a file of them), in either format:
    DMCP5 3.58's serial console (NAV47 make USB=sercon): the LCD rows on channel 0; a frame when the rows stop coming
    for `wait` seconds. NAV47 / Chess 47 over TinyUSB (make USB=tinyusb): "NAV47 LCD 400 240", 240 hex lines, "END"."""
    import select
    f, fd = opened or open_port(path)
    d = Dmcp(raw)
    if fd is not None:                                  # ask DMCP who is there, and for the whole screen
        os.write(fd, b'\x10V\n\x10L\n')
        d.clear()
    rows, inside = [], False
    buf = bytearray()
    dirty = False
    while True:
        if fd is not None:
            r, _, _ = select.select([fd] + ([sys.stdin] if typing else []), [], [], wait)
            if typing and sys.stdin in r:                # --keys: the PC keyboard
                typing.feed(os.read(sys.stdin.fileno(), 64))
                r = [x for x in r if x is not sys.stdin]
                if not r:
                    continue
            chunk = os.read(fd, 65536) if r else b''
            if not r and dirty:
                dirty = False
                yield d.px, d
                continue
            if r and not chunk:                         # the cable went out
                raise FileNotFoundError(path)
        else:
            chunk = f.read(65536)
            if not chunk:
                if dirty:
                    yield d.px, d
                return
        for b in chunk:
            if 0x10 <= b <= 0x1a:                       # a channel id: never data
                d.ch = b - 0x10
            elif b == 0x0a:
                line = bytes(buf)
                buf.clear()
                if d.line(line):
                    dirty = True
                    continue
                t = line.strip().decode('ascii', 'replace')
                if t.endswith('LCD 400 240'):
                    rows, inside = [], True
                elif t == 'END' and inside:
                    inside = False
                    if len(rows) == H:
                        yield rows, None
                elif inside and len(t) == W // 4:
                    v = int(t, 16)
                    rows.append([(v >> (W - 1 - x)) & 1 for x in range(W)])
            else:
                buf.append(b)


# ---------------------------------------------------------------- the keys (DMCP5 3.58's serial console, channel 0)
# DMCP's key codes (dmcp.h): the DM42n's keys, top-left to bottom-right, F1-F6 above them
KEYS = {'SIGMA': 1, 'INV': 2, 'SQRT': 3, 'LOG': 4, 'LN': 5, 'XEQ': 6, 'STO': 7, 'RCL': 8, 'RDN': 9, 'SIN': 10,
        'COS': 11, 'TAN': 12, 'ENTER': 13, 'SWAP': 14, 'CHS': 15, 'E': 16, 'BSP': 17, 'UP': 18, '7': 19, '8': 20,
        '9': 21, 'DIV': 22, 'DOWN': 23, '4': 24, '5': 25, '6': 26, 'MUL': 27, 'SHIFT': 28, '1': 29, '2': 30, '3': 31,
        'SUB': 32, 'EXIT': 33, '0': 34, 'DOT': 35, 'RUN': 36, 'ADD': 37,
        'F1': 38, 'F2': 39, 'F3': 40, 'F4': 41, 'F5': 42, 'F6': 43}
ALIAS = {'+': 'ADD', '-': 'SUB', '*': 'MUL', '/': 'DIV', '.': 'DOT', 'EEX': 'E', 'R/S': 'RUN', 'RS': 'RUN',
         'X<>Y': 'SWAP', 'XY': 'SWAP', 'RDOWN': 'RDN', 'R↓': 'RDN', '1/X': 'INV', 'SUM': 'SIGMA', 'Σ+': 'SIGMA',
         'BACK': 'BSP', '←': 'BSP', 'ESC': 'EXIT', 'F': 'SHIFT'}


def key_code(name):
    """a key name (ENTER, 7, SHIFT, F1, +, ...) -> DMCP's key code; 'SHIFT+UP' -> two keys held together"""
    n = name.upper()
    if '+' in n and len(n) > 1:
        a, b = n.split('+', 1)
        return (key_code(b) << 8) | key_code(a)                      # KPhhhh: k1 the low byte
    n = ALIAS.get(n, n)
    if n not in KEYS:
        raise ValueError('unknown key %r (keys: %s)' % (name, ' '.join(KEYS)))
    return KEYS[n]


class Keys:
    """presses keys through DMCP's channel 0: KPhh (held down) ... KR (released), as DMCP's own commands"""

    def __init__(self, fd, hold=0.06, gap=0.12):
        self.fd, self.hold, self.gap = fd, hold, gap

    def press(self, code):
        hexa = '%04X' % code if code > 0xFF else '%02X' % code
        os.write(self.fd, b'\x10KP' + hexa.encode() + b'\n')
        time.sleep(self.hold)
        os.write(self.fd, b'\x10KR\n')
        time.sleep(self.gap)

    def send(self, tokens):
        """key names; WAIT:ms waits"""
        for t in tokens:
            if t.upper().startswith('WAIT:'):
                time.sleep(int(t[5:]) / 1000.0)
            else:
                self.press(key_code(t))


# the PC keyboard in --keys: digits, . + - * /, Enter, Backspace, Esc (EXIT), the arrows (UP DOWN), Tab (SHIFT),
# F1-F6; ':' then a key name and Enter for any other key (:SIN, :STO, :XEQ, :SHIFT+UP ...); Ctrl+C or Ctrl+Q ends
PCKEYS = {b'\r': 'ENTER', b'\n': 'ENTER', b'\x7f': 'BSP', b'\x08': 'BSP', b'\t': 'SHIFT', b'\x1b': 'EXIT',
          b'\x1b[A': 'UP', b'\x1b[B': 'DOWN', b'\x1bOP': 'F1', b'\x1bOQ': 'F2', b'\x1bOR': 'F3', b'\x1bOS': 'F4',
          b'\x1b[15~': 'F5', b'\x1b[17~': 'F6', b'\x1b[11~': 'F1', b'\x1b[12~': 'F2', b'\x1b[13~': 'F3',
          b'\x1b[14~': 'F4', b'+': 'ADD', b'-': 'SUB', b'*': 'MUL', b'/': 'DIV', b'.': 'DOT', b',': 'DOT',
          b'e': 'E', b'E': 'E'}
PCKEYS.update({str(d).encode(): str(d) for d in range(10)})


class Typing:
    """--keys: the PC keyboard -> the DM42n's keys"""

    def __init__(self, keys):
        self.keys = keys
        self.name = None                                 # after ':': a key name being typed
        self.last = ''

    def feed(self, data):
        i = 0
        while i < len(data):
            if data[i] in (3, 17):                       # Ctrl+C, Ctrl+Q
                raise KeyboardInterrupt
            if self.name is not None:
                c = data[i:i + 1]
                i += 1
                if c in (b'\r', b'\n'):
                    try:
                        self.keys.press(key_code(self.name)); self.last = self.name.upper()
                    except ValueError:
                        self.last = '%s? no such key' % self.name
                    self.name = None
                elif c in (b'\x7f', b'\x08'):
                    self.name = self.name[:-1]
                elif c == b'\x1b':
                    self.name = None
                else:
                    self.name += c.decode('ascii', 'ignore')
                continue
            if data[i:i + 1] == b':':
                self.name = ''; i += 1
                continue
            for n in (5, 3, 1):                          # the longest known sequence first (escape sequences)
                k = PCKEYS.get(data[i:i + n])
                if k:
                    self.keys.press(KEYS[k]); self.last = k
                    i += n
                    break
            else:
                i += 1

    def status(self):
        return (':' + self.name + '_') if self.name is not None else ('key ' + self.last if self.last else '')


def braille(px, x0, y0, w, h, scale):
    """Unicode Braille: 2 x 4 dots a character; scale > 1 repeats each pixel."""
    out = []
    sw, sh = w * scale, h * scale
    get = lambda x, y: px[y0 + y // scale][x0 + x // scale] if 0 <= y < sh and 0 <= x < sw else 0
    bits = ((0, 0, 0x01), (0, 1, 0x02), (0, 2, 0x04), (1, 0, 0x08), (1, 1, 0x10), (1, 2, 0x20), (0, 3, 0x40), (1, 3, 0x80))
    for cy in range(0, sh, 4):
        out.append(''.join(chr(0x2800 + sum(m for dx, dy, m in bits if get(cx + dx, cy + dy)))
                           for cx in range(0, sw, 2)))
    return out


def blocks(px, x0, y0, w, h):
    ch = {(0, 0): ' ', (1, 0): '\u2580', (0, 1): '\u2584', (1, 1): '\u2588'}
    return [''.join(ch[(px[y][x], px[y + 1][x] if y + 1 < y0 + h else 0)] for x in range(x0, x0 + w))
            for y in range(y0, y0 + h, 2)]


def main():
    ap = argparse.ArgumentParser(description='The DM42n screen (NAV47 / Chess 47 USB) in the terminal.')
    ap.add_argument('port', nargs='?', default=None,
                    help='serial port, or a file of frames (default: the DM42n found in /dev/serial/by-id)')
    ap.add_argument('--zoom', help='x,y,w,h: show only this part, as big as the terminal allows')
    ap.add_argument('--blocks', action='store_true', help='half blocks instead of Braille dots')
    ap.add_argument('--png', help='save every frame as a PNG in this folder')
    ap.add_argument('--scale', type=int, default=3, help='PNG pixel size (default 3)')
    ap.add_argument('--once', action='store_true', help='one frame, then exit')
    ap.add_argument('--latest', help='write each frame to this one PNG file (for an image viewer that reloads it)')
    ap.add_argument('--quiet', action='store_true', help='no picture in the terminal (with --png / --latest)')
    ap.add_argument('--raw', action='store_true', help='print every line received (channel, ASCII part, binary bytes)')
    ap.add_argument('--wait', type=float, default=0.08, help='serial console: a frame after this many s without rows')
    ap.add_argument('--keys', action='store_true',
                    help='type on the DM42n: digits . + - * / Enter Backspace Esc(EXIT) arrows Tab(SHIFT) F1-F6; '
                         '":NAME" Enter for any key (:SIN :STO :XEQ :SHIFT+UP); Ctrl+C ends')
    ap.add_argument('--send', help='press these keys first: names with spaces, e.g. "SHIFT 7 ENTER WAIT:500 F1"')
    ap.add_argument('--c47', help='C47 commands to key in first (tools/c47keys.py): e.g. "2 ENTER 3 + SIN STO 01"')
    ap.add_argument('--c43', help='--c47: the C47 firmware source with the keyboard (default $C43_DIR or ~/opt/c43)')
    ap.add_argument('--kbd', help='--c47: the keyboard table, kbd_std_NAME of newer firmware (default C47)')
    ap.add_argument('--hold', type=float, default=0.06, help='s a key is held down (default 0.06)')
    ap.add_argument('--gap', type=float, default=0.12, help='s between keys (default 0.12)')
    a = ap.parse_args()
    if not a.port:                                  # the DM42n by name: ttyACM0, ACM1 ... whichever it got
        import glob
        found = sorted(glob.glob('/dev/serial/by-id/*SwissMicros*') + glob.glob('/dev/serial/by-id/*DM42*'))
        a.port = found[0] if found else '/dev/ttyACM0'
        print('port', a.port)
    x0, y0, w, h = map(int, a.zoom.split(',')) if a.zoom else (0, 0, W, H)
    n = 0
    opened = typing = term = None
    try:
        if a.keys or a.send or a.c47:                    # the keys go over DMCP5 3.58's serial console
            opened = open_port(a.port)
            if opened[1] is None:
                sys.exit('--keys / --send / --c47 need the serial port, not a file')
            keys = Keys(opened[1], a.hold, a.gap)
            if a.c47:
                sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
                import c47keys
                seq = c47keys.translate(a.c47, c47keys.layout(a.c43, a.kbd))
                print('C47:', a.c47, '->', ' '.join(seq))
                keys.send(seq)
            if a.send:
                keys.send(a.send.split())
            if not (a.keys or a.once or a.png or a.latest):
                return
            if a.keys:
                import termios, tty
                term = termios.tcgetattr(sys.stdin)
                tty.setcbreak(sys.stdin.fileno())
                typing = Typing(keys)
        for px, dm in frames(a.port, a.raw, a.wait, opened, typing):
            n += 1
            if not a.quiet:
                cols, lines = shutil.get_terminal_size((200, 62))
                if a.blocks:
                    art = blocks(px, x0, y0, w, h)
                else:
                    scale = max(1, min((cols * 2) // w, ((lines - 2) * 4) // h))
                    art = braille(px, x0, y0, w, h, scale)
                sys.stdout.write('\x1b[H\x1b[2J' + '\n'.join(art) + '\n')
            sys.stdout.write(time.strftime('%H:%M:%S') + '  frame %d' % n +
                             ('  serial console, %d rows  %s' % (dm.rows, dm.ident or '') if dm else '  TinyUSB') +
                             ('   [%s]' % typing.status() if typing else '') + '\n')
            sys.stdout.flush()
            if a.png or a.latest:
                from PIL import Image
                img = Image.new('RGB', (W, H), (223, 230, 214))
                p = img.load()
                for y in range(H):
                    for x in range(W):
                        if px[y][x]:
                            p[x, y] = (30, 34, 32)
                img = img.crop((x0, y0, x0 + w, y0 + h)).resize((w * a.scale, h * a.scale), Image.NEAREST)
                if a.png:
                    os.makedirs(a.png, exist_ok=True)
                    img.save(os.path.join(a.png, time.strftime('dm42_%Y%m%d_%H%M%S') + '_%03d.png' % n))
                if a.latest:
                    tmp = a.latest + '.tmp.png'
                    img.save(tmp)
                    os.replace(tmp, a.latest)      # whole files only: the viewer never reads half a picture
            if a.once:
                break
    except KeyboardInterrupt:
        pass
    except PermissionError:
        sys.exit('no access to %s: add yourself to the uucp (Arch) or dialout (Debian) group' % a.port)
    except FileNotFoundError:
        sys.exit('%s not found: on the DM42n press >> >> USB in NAV47 or Chess 47 (the USB cable connected)' % a.port)
    except OSError as ex:
        sys.exit('%s: %s' % (a.port, ex))
    finally:
        if term is not None:
            import termios
            termios.tcsetattr(sys.stdin, termios.TCSADRAIN, term)


if __name__ == '__main__':
    main()
