#!/usr/bin/env python3
"""dm42port.py - the DM42n's serial port for the PC tools (usb_stockfish.py, lichess_board.py): Chess 47's text lines,
whichever USB the .pg5 was built with.

  DMCP5 3.58's serial console (make, the default): one byte stream, channels. A byte 0x10 + n switches to channel n;
  Chess 47's lines are on channel 2, DMCP's own (LCD rows, key reports, V) on channel 0. We send each line as 0x12
  + the line + '\\n' (DMCP passes channel 2 to the program); a line is ASCII, a binary part has the top bit set.
  TinyUSB (make USB=tinyusb): plain text lines, with the screen frames ("CHESS47 LCD 400 240" ... "END") among them.
  The 0x12 we send is not a printable character: the TinyUSB build drops it. So the same tools work with both.

  port = Port(path)            # opens it raw, read and write (opening raises DTR: DMCP's "connected")
  port.send('e7e5')            # one line to Chess 47
  for line in port.lines():    # Chess 47's lines, without '\\r\\n'; the screen and DMCP's own lines skipped
      ...
  find()                       # the DM42n in /dev/serial/by-id, else /dev/ttyACM0

The screen at the same time (one program has the port: the bridge itself shows it):
  screen = Screen(png='/tmp/dm42.png', view=True)   # a PNG rewritten on each change (feh -R 0.5 it), and / or
  port = Port(path, screen)                         # the screen in Braille in this terminal, the log under it
  screen.log('text')                                # a log line (with view: under the screen, else printed)
"""
import collections
import glob
import os
import shutil
import sys
import termios
import threading
import time

CH = 2                                   # Chess 47's channel on the serial console


def find():
    found = sorted(glob.glob('/dev/serial/by-id/*SwissMicros*') + glob.glob('/dev/serial/by-id/*DM42*'))
    return found[0] if found else '/dev/ttyACM0'


class Screen:
    """the DM42n's screen from the same stream: DMCP's LCD rows (channel 0) or TinyUSB's frames"""

    def __init__(self, png=None, view=False, scale=3, every=0.3):
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import dm42view
        self.V = dm42view
        self.dm = dm42view.Dmcp()
        self.png, self.view, self.scale, self.every = png, view, scale, every
        self.logs = collections.deque(maxlen=12)
        self.status = ''                     # --keys: the last key typed
        self.dirty = False
        self.lock = threading.Lock()
        threading.Thread(target=self._run, daemon=True).start()

    def row(self, raw):                      # a line of channel 0: an LCD row (or V, key reports: ignored)
        self.dm.ch = 0
        if self.dm.line(raw):
            self.dirty = True

    def frame(self, px):                     # a whole TinyUSB frame
        self.dm.px = px
        self.dirty = True

    def log(self, text):
        if self.view:
            with self.lock:
                self.logs.append(time.strftime('%H:%M:%S ') + text)
            self.dirty = True
        else:
            print(text, flush=True)

    def _run(self):
        while True:
            time.sleep(self.every)
            if not self.dirty:
                continue
            self.dirty = False
            px = [r[:] for r in self.dm.px]
            if self.png:
                try:
                    from PIL import Image
                    img = Image.new('RGB', (400, 240), (223, 230, 214))
                    p = img.load()
                    for y in range(240):
                        for x in range(400):
                            if px[y][x]:
                                p[x, y] = (30, 34, 32)
                    img = img.resize((400 * self.scale, 240 * self.scale), Image.NEAREST)
                    tmp = self.png + '.tmp.png'
                    img.save(tmp)
                    os.replace(tmp, self.png)
                except ImportError:
                    self.png = None
                    self.log('--screen needs Pillow (pip install pillow)')
            if self.view:
                cols, lines = shutil.get_terminal_size((200, 62))
                with self.lock:
                    logs = list(self.logs)
                room = max(4, lines - len(logs) - 2)
                sc = max(1, min((cols * 2) // 400, (room * 4) // 240))
                art = self.V.braille(px, 0, 0, 400, 240, sc) if sc > 1 or room * 4 >= 240 else \
                    self.V.braille(px, 0, 0, 400, 240, 1)[:room]
                sys.stdout.write('\x1b[H\x1b[2J' + '\n'.join(art) + '\n' + '\n'.join(logs) + '\n' +
                                 ('[%s]\n' % self.status if self.status else ''))
                sys.stdout.flush()


def keyboard(port, screen=None, hold=0.06, gap=0.12):
    """--keys: the PC keyboard types on the calculator while a bridge runs (dm42view.py's map: digits . + - * /,
    Enter, Backspace, Esc = EXIT, arrows, Tab = SHIFT, F1-F6, ':NAME' Enter for any key); a thread; the terminal
    goes back to normal at the end. Needs DMCP5 3.58's serial console (the TinyUSB build takes no keys)."""
    import atexit
    import termios
    import tty
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import dm42view
    if not port.tty or not sys.stdin.isatty():
        return None
    typing = dm42view.Typing(dm42view.Keys(port.fd, hold, gap))
    port.typing = typing
    saved = termios.tcgetattr(sys.stdin)
    atexit.register(termios.tcsetattr, sys.stdin, termios.TCSADRAIN, saved)
    tty.setcbreak(sys.stdin.fileno())

    def run():
        while True:
            data = os.read(sys.stdin.fileno(), 64)
            if not data:
                return
            try:
                typing.feed(data)
            except OSError:                              # the port is closed just now: the key is lost
                pass
            except KeyboardInterrupt:                    # Ctrl+C / Ctrl+Q in cbreak mode: end the bridge
                termios.tcsetattr(sys.stdin, termios.TCSADRAIN, saved)
                os._exit(0)
            if screen and screen.view:
                screen.status = typing.status()
                screen.dirty = True
    threading.Thread(target=run, daemon=True).start()
    return typing


class Port:
    """path None: the DM42n wherever it is (find(), again after every reconnection). The port is waited for when
    it is not there (the calculator's USB off, the cable out): Ctrl+C stops the wait."""

    def __init__(self, path, screen=None, say=print):
        self.want = path
        self.screen = screen
        self.say = say
        self.typing = None                               # keyboard(): its keys follow a reopened port
        self.fd = None
        self.tty = True
        if path and os.path.exists(path) and not path.startswith('/dev/'):     # a recorded stream (acm.out)
            self.path = path
            self.fd = os.open(path, os.O_RDONLY)
            self.tty = False
        else:
            self._open()
        self.console = False                             # seen a channel byte: DMCP's serial console
        self.ch = 0

    def _open(self):
        told = False
        while True:
            self.path = self.want or find()
            try:
                self.fd = os.open(self.path, os.O_RDWR | os.O_NOCTTY)
                break
            except (FileNotFoundError, OSError) as ex:
                if isinstance(ex, PermissionError):
                    raise SystemExit('no access to %s: add yourself to the uucp (Arch) or dialout (Debian) group'
                                     % self.path)
                if not told:
                    self.say('Waiting for the calculator on %s: press >> >> USB on the DM42n (Ctrl+C quits)'
                             % (self.want or '/dev/ttyACM*'))
                    told = True
                time.sleep(1)
        t = termios.tcgetattr(self.fd)
        t[0] = 0; t[1] = 0; t[3] = 0                     # raw: no echo back, no line editing, no translation
        t[2] |= termios.CLOCAL | termios.CREAD
        termios.tcsetattr(self.fd, termios.TCSANOW, t)
        if told:
            self.say('The calculator is back on %s' % self.path)

    def reopen(self):
        """after the port closed: wait for it again (a new /dev/ttyACM* too)"""
        try:
            os.close(self.fd)
        except OSError:
            pass
        time.sleep(1)
        self._open()
        self.console, self.ch = False, 0
        if self.typing:
            self.typing.keys.fd = self.fd

    def send(self, line):
        """one line to Chess 47: 0x12 first (the serial console's channel 2; dropped by the TinyUSB build)"""
        if self.tty:
            try:
                os.write(self.fd, bytes([0x10 + CH]) + line.encode('ascii') + b'\n')
            except OSError:                              # the port just closed: lines() ends, then reopen()
                pass

    def ask_screen(self):
        """the whole screen from DMCP (L on channel 0): asked once when a Screen is attached"""
        if self.tty:
            try:
                os.write(self.fd, b'\x10L\n')
            except OSError:
                pass

    def lines(self):
        """Chess 47's lines; ends when the port closes (the cable out) or the file ends"""
        buf = bytearray()
        frame = False
        rows = []
        if self.screen:
            self.ask_screen()
        while True:
            try:
                chunk = os.read(self.fd, 4096)
            except OSError:                              # the device went away (USB off, the cable out)
                return
            if not chunk:
                return
            for b in chunk:
                if 0x10 <= b <= 0x1a:                    # a channel id: never data
                    self.console = True
                    self.ch = b - 0x10
                    continue
                if b != 0x0a:
                    buf.append(b)
                    continue
                raw = bytes(buf)
                buf.clear()
                if self.console and self.ch != CH:       # DMCP's own lines (the LCD rows, V, key reports)
                    if self.screen and self.ch == 0:
                        self.screen.row(raw)
                    continue
                line = bytes(c for c in raw if c < 0x80).strip().decode('ascii', 'replace')
                if line.endswith('LCD 400 240'):         # TinyUSB: a screen frame, to its END
                    frame, rows = True, []
                elif frame:
                    frame = line != 'END'
                    if frame and len(line) == 100:
                        v = int(line, 16)
                        rows.append([(v >> (399 - x)) & 1 for x in range(400)])
                    elif not frame and self.screen and len(rows) == 240:
                        self.screen.frame(rows)
                elif line:
                    yield line
