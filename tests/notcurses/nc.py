"""Helpers for the lace-nc tests"""
import threading
import time

from harness import LACE_NC, Tui


class Nc(Tui):
    def __init__(self, home, args, **kw):
        kw.setdefault("binary", LACE_NC)
        super().__init__(home, args, **kw)
        # notcurses waits for a primary device attributes reply, which a
        # detached tmux never sends: answer it once the binary is running
        # (the bytes wait in the tty buffer until notcurses reads them)
        threading.Timer(0.15, self._da1).start()

    def _da1(self):
        try:
            self.tmux("send-keys", "-t", "t", "-l", "\x1b[?62;c")
        except Exception:  # noqa: BLE001  (the test may already be over)
            pass

    def click(self, x, y, double=False, wheel=None, button=0):
        """A mouse press (and release) at screen cell x, y as an SGR report,
        the mode notcurses asks the terminal for (the harness's X10 reports
        are ignored); wheel="up"/"down" scrolls instead; button 1 is the
        middle one, 2 the right one"""
        b = {None: button, "up": 64, "down": 65}[wheel]
        for _ in range(2 if double and wheel is None else 1):
            self.type(f"\x1b[<{b};{x + 1};{y + 1}M")
            if wheel is None:
                self.type(f"\x1b[<{b};{x + 1};{y + 1}m")
        time.sleep(0.1)

    def drag(self, x, y0, y1):
        """Press at (x, y0), move to row y1 with the button held, release"""
        self.type(f"\x1b[<0;{x + 1};{y0 + 1}M")
        step = 1 if y1 >= y0 else -1
        for y in range(y0 + step, y1 + step, step):
            self.type(f"\x1b[<32;{x + 1};{y + 1}M")
        self.type(f"\x1b[<0;{x + 1};{y1 + 1}m")
        time.sleep(0.1)

    def drag_path(self, points):
        """Press at the first (x, y), move with the button held through the
        rest, release at the last"""
        x, y = points[0]
        self.type(f"\x1b[<0;{x + 1};{y + 1}M")
        for x, y in points[1:]:
            self.type(f"\x1b[<32;{x + 1};{y + 1}M")
        self.type(f"\x1b[<0;{x + 1};{y + 1}m")
        time.sleep(0.1)
