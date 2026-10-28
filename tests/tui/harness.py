"""Drive the lace TUI in a real terminal (tmux) and read its screen.

Each Tui runs in its own tmux server (`tmux -L <name>`), never the user's,
with its own HOME, so config.json, session.json and the daemon socket are
private to the test. The daemon runs over stdio unless a test asks for a
socket daemon, so it ends with the TUI.
"""

import json
import os
import re
import shutil
import sqlite3
import subprocess
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LACE = os.path.join(ROOT, "tui", "ncurses", "build", "lace")
LACE_NC = os.path.join(ROOT, "tui", "notcurses", "build", "lace-nc")
LACED_DIR = os.path.join(ROOT, "laced", "build")


class TuiError(AssertionError):
    pass


def config_dir(home):
    """Where the TUI keeps config.json and session.json for this HOME"""
    if os.uname().sysname == "Darwin":
        return os.path.join(home, "Library", "Application Support", "lace")
    return os.path.join(home, ".config", "lace")


def socket_dir():
    """Where tmux puts the sockets of named servers (`tmux -L`)"""
    tmpdir = os.environ.get("TMUX_TMPDIR") or "/tmp"
    return os.path.join(tmpdir, f"tmux-{os.getuid()}")


class Home:
    """A private HOME with a config.json (stdio daemon by default)"""

    def __init__(self, general=None):
        # Under /tmp (as on Linux): macOS's $TMPDIR is a long path, which the
        # screens clip, and paths on them must stay recognisable
        self.path = tempfile.mkdtemp(prefix="lace-tui-", dir="/tmp" if os.path.isdir("/tmp") else None)
        self.cfg_dir = config_dir(self.path)
        os.makedirs(self.cfg_dir, exist_ok=True)
        settings = {"daemon_spawn_mode": 1, "restore_session": True,
                    "quit_confirmation": False, "delete_confirmation": True}
        settings.update(general or {})
        self.write("config.json", {"general": settings})
        self.tuis = []  # every Tui started in this HOME, closed by close_tuis()

    def write(self, name, data):
        with open(os.path.join(self.cfg_dir, name), "w") as f:
            json.dump(data, f)

    def read(self, name):
        p = os.path.join(self.cfg_dir, name)
        if not os.path.exists(p):
            return None
        with open(p) as f:
            return json.load(f)

    def file(self, name):
        return os.path.join(self.path, name)

    def close_tuis(self):
        """Kill the tmux server of every Tui started here that is still
        running: a test that fails before its own close() must not leave
        a server, its TUI and its daemon behind"""
        for t in self.tuis:
            t.close()

    def cleanup(self):
        self.close_tuis()
        shutil.rmtree(self.path, ignore_errors=True)


def sqlite_db(path, script):
    """Create a SQLite database from SQL"""
    con = sqlite3.connect(path)
    con.executescript(script)
    con.commit()
    con.close()
    return path


def sqlite_rows(path, sql, args=()):
    con = sqlite3.connect(path)
    try:
        return con.execute(sql, args).fetchall()
    finally:
        con.close()


def sqlite_exec(path, sql, args=()):
    """Change the database behind the TUI's back (committed)"""
    con = sqlite3.connect(path)
    try:
        con.execute(sql, args)
        con.commit()
    finally:
        con.close()


class Tui:
    """One lace TUI in a private tmux server"""

    def __init__(self, home, args, cols=120, rows=30, env=None, binary=None):
        self.home = home
        self.server = f"lace-tui-{os.getpid()}-{int(time.time() * 1000) % 100000000}"
        self.cols, self.rows = cols, rows
        # A UTF-8 locale that exists: macOS has no C.UTF-8, minimal Linux
        # systems often only C.UTF-8
        utf8 = "en_US.UTF-8" if os.uname().sysname == "Darwin" else "C.UTF-8"
        e = {"HOME": home.path, "LANG": utf8, "LC_ALL": utf8, "ESCDELAY": "25",
             "PATH": LACED_DIR + os.pathsep + os.environ.get("PATH", ""),
             "LACED_LOG": home.file("laced.log")}
        e.update(env or {})
        exports = " ".join(f"{k}={shquote(v)}" for k, v in e.items())
        argv = " ".join(shquote(a) for a in [binary or LACE, *args])
        # The shell stays after the TUI so its exit status can be read
        cmd = f"env {exports} {argv}; echo __LACE_EXIT__=$?; sleep 3600"
        self.tmux("new-session", "-d", "-s", "t", "-x", str(cols), "-y",
                  str(rows), cmd)
        self.exited = None
        self.closed = False
        home.tuis.append(self)

    # -- tmux ---------------------------------------------------------------

    def tmux(self, *args, capture=False):
        cmd = ["tmux", "-L", self.server, "-f", os.devnull, *args]
        r = subprocess.run(cmd, check=False, text=True,
                           stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
                           stderr=subprocess.PIPE)
        if r.returncode != 0 and args[0] not in ("kill-server",):
            raise TuiError(f"tmux {' '.join(args)}: {r.stderr.strip()}")
        return r.stdout if capture else None

    def close(self):
        """Kill the tmux server (and with it the TUI and its stdio daemon);
        safe to call twice. tmux leaves the socket file behind, so it is
        removed here"""
        if self.closed:
            return
        self.closed = True
        self.tmux("kill-server")
        sock = os.path.join(socket_dir(), self.server)
        try:
            os.unlink(sock)
        except OSError:
            pass

    def pid(self):
        """Process id of the lace process (the pane's shell runs it)"""
        shell = self.tmux("display", "-p", "-t", "t", "#{pane_pid}", capture=True).strip()
        return subprocess.run(["pgrep", "-P", shell], text=True,
                              stdout=subprocess.PIPE).stdout.split()[0]

    def child_daemons(self):
        """Process ids of laced processes lace started as its children (a
        private stdio daemon; a socket daemon is detached, not a child)"""
        out = subprocess.run(["pgrep", "-P", self.pid(), "laced"], text=True,
                             stdout=subprocess.PIPE).stdout
        return [int(p) for p in out.split()]

    def cpu_seconds(self):
        """CPU time the lace process has used so far"""
        pid = self.pid()
        t = subprocess.run(["ps", "-o", "time=", "-p", pid], text=True,
                           stdout=subprocess.PIPE).stdout.strip()
        days, _, t = t.rpartition("-")
        secs = 0.0
        for part in t.split(":"):
            secs = secs * 60 + float(part)
        return secs + (int(days) * 86400 if days else 0)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    # -- common steps -------------------------------------------------------

    def open_table(self, name):
        """Open a table from the sidebar through its filter (works at
        startup, where the sidebar has focus)"""
        self.wait("TABLES")
        self.keys("/")
        self.type(name)
        self.keys("Enter", "Enter")
        return self.wait_header(self.POSITION)

    # -- input --------------------------------------------------------------

    def keys(self, *keys, delay=0.05):
        """tmux key names: Enter, Escape, Up, PPage, C-r, BSpace, Tab, BTab, ..."""
        for k in keys:
            self.tmux("send-keys", "-t", "t", k)
            time.sleep(delay)

    def type(self, text):
        self.tmux("send-keys", "-t", "t", "-l", text)
        time.sleep(0.05)

    def click(self, x, y, double=False, wheel=None, button=0):
        """Mouse at screen column x, row y (0-based), as an xterm X10 report:
        a click (button 0 left, 1 middle; two for double), or
        wheel="up"/"down" """
        def report(b):
            return "\x1b[M" + chr(32 + b) + chr(33 + x) + chr(33 + y)
        if wheel:
            self.type(report(64 if wheel == "up" else 65))
            return
        for _ in range(2 if double else 1):
            self.type(report(button))
            self.type(report(3))
        time.sleep(0.1)

    def find(self, text):
        """(x, y) of the first place text is on screen, or None"""
        for y, line in enumerate(self.screen()):
            x = line.find(text)
            if x >= 0:
                return x, y
        return None

    def resize(self, cols, rows):
        self.tmux("resize-window", "-t", "t", "-x", str(cols), "-y", str(rows))
        self.cols, self.rows = cols, rows

    # -- screen -------------------------------------------------------------

    def screen(self):
        return self.tmux("capture-pane", "-t", "t", "-p", capture=True).split("\n")[: self.rows]

    def text(self):
        return "\n".join(self.screen())

    def status(self):
        """The bottom line (status bar)"""
        lines = [l for l in self.screen()]
        return lines[self.rows - 1] if len(lines) >= self.rows else ""

    # The cursor cell's colours: the theme's (its cursor background, dark
    # 45475a or light bcc0cc, as the nearest of the 256 colours), or black
    # on cyan on a terminal of fewer colours
    CURSOR = re.compile(r"\x1b\[30m\x1b\[46m|48;5;(?:239|251)m")
    # Backgrounds (the nearest of the 256 colours, dark and light theme):
    # the grid's header row, the cursor row and the cursor cell (accent)
    BG_HEADER = {237, 252}
    BG_CURSOR = {239, 251}
    BG_ACCENT = {116, 38}
    SGR = re.compile(r"\x1b\[([0-9;:]*)m")

    @classmethod
    def _cells(cls, line):
        """A captured line as (character, background) pairs; the background
        is a 256-colour index, or None for the terminal's"""
        out, bg, pos = [], None, 0
        for m in cls.SGR.finditer(line):
            out.extend((ch, bg) for ch in line[pos:m.start()])
            pos = m.end()
            ps = [int(p) if p.isdigit() else 0 for p in re.split(r"[;:]", m.group(1) or "0")]
            i = 0
            while i < len(ps):
                if ps[i] == 0 or ps[i] == 49:
                    bg = None
                elif ps[i] in (38, 48) and i + 2 < len(ps) and ps[i + 1] == 5:
                    if ps[i] == 48:
                        bg = ps[i + 2]
                    i += 2
                elif ps[i] in (38, 48) and i + 4 < len(ps) and ps[i + 1] == 2:
                    i += 4
                elif 40 <= ps[i] <= 47:
                    bg = ps[i] - 40
                i += 1
        out.extend((ch, bg) for ch in line[pos:])
        return out

    def _grid(self):
        """(header cells, cursor row cells, x0, x1) of the grid that holds
        the cursor: its header row is the one in the header colour above a
        row whose cell at the grid's left edge has the cursor's or the
        accent colour; None when there is none"""
        raw = self.tmux("capture-pane", "-t", "t", "-p", "-e", capture=True).split("\n")[: self.rows]
        rows = [self._cells(l) for l in raw]
        for h in range(len(rows) - 2, 1, -1):
            xs = [i for i, (_, bg) in enumerate(rows[h]) if bg in self.BG_HEADER]
            if not xs:
                continue
            x0 = xs[0]
            for r in rows[h + 1:len(rows) - 1]:
                if len(r) > x0 and (r[x0][1] in self.BG_CURSOR or r[x0][1] in self.BG_ACCENT):
                    # tmux leaves out trailing blanks: the row ends where its text does
                    return rows[h], r, x0, max(len(rows[h]), len(r))
        return None

    def cursor_row_text(self):
        """Text of the grid row holding the cursor, from the grid's left edge"""
        g = self._grid()
        if not g:
            return None
        _, row, x0, x1 = g
        return "".join(ch for ch, _ in row[x0:x1])

    MARKS = "\u26b7\u21d2"  # a key's and a foreign key's marks after a name

    def cursor_cells(self):
        """The cells of the cursor row, split where the grid's header starts
        each column's name, stripped"""
        g = self._grid()
        if not g:
            return None
        head, row, x0, x1 = g
        names = "".join(ch for ch, _ in head[x0:x1])
        starts = [i for i, ch in enumerate(names)
                  if ch != " " and (i == 0 or names[i - 1] == " ") and ch not in self.MARKS]
        text = "".join(ch for ch, _ in row[x0:x1])
        bounds = starts + [len(text)]
        return [text[a:b].strip() for a, b in zip(bounds, bounds[1:])]

    # The header's cursor position: "3 of 1,200", "1 of \u2248200", "1 of 1,000+"
    POSITION = re.compile(r"([\d,]+) of \u2248?([\d,]+)\+?")

    def row_number(self):
        m = self.POSITION.search(self.header())
        return int(m.group(1).replace(",", "")) if m else None

    def wait(self, what, timeout=10.0, where=None):
        """Wait until what (a string or regex, or a predicate on the screen
        text) appears; returns the regex match or True"""
        deadline = time.time() + timeout
        last = ""
        while time.time() < deadline:
            last = where() if where else self.text()
            if callable(what):
                if what(last):
                    return True
            else:
                m = re.search(what if isinstance(what, re.Pattern) else re.escape(what), last)
                if m:
                    return m
            if "__LACE_EXIT__=" in self.text() and not self._expect_exit:
                break
            time.sleep(0.05)
        raise TuiError(f"timed out waiting for {what!r}\n--- screen ---\n{self.text()}")

    _expect_exit = False

    def wait_status(self, what, timeout=10.0):
        return self.wait(what, timeout, where=self.status)

    def wait_stable(self, quiet=0.4, timeout=10.0):
        """Wait until the screen has not changed for quiet seconds (the keys
        sent are all handled)"""
        end = time.time() + timeout
        last, since = self.text(), time.time()
        while time.time() < end:
            time.sleep(0.05)
            now = self.text()
            if now != last:
                last, since = now, time.time()
            elif time.time() - since >= quiet:
                return
        raise TuiError(f"the screen kept changing\n{last}")

    def header(self):
        """The top line: the path to the current tab, the sorts and filters
        and the cursor position ("3 of 1,200")"""
        lines = self.screen()
        return lines[0] if lines else ""

    def wait_header(self, what, timeout=10.0):
        return self.wait(what, timeout, where=self.header)

    def workspace(self):
        """The current workspace's name: the header's first segment"""
        return self.header().strip().split(" \u25b8 ")[0].strip()

    def wait_workspace(self, name, timeout=10.0):
        return self.wait(lambda s: self.workspace() == name, timeout)

    def quit(self, timeout=10.0):
        """Quit with q from wherever the main screen is; returns the exit code"""
        self._expect_exit = True
        self.keys("Escape", "Escape")
        self.keys("q")
        m = self.wait(re.compile(r"__LACE_EXIT__=(\d+)"), timeout)
        self.exited = int(m.group(1))
        return self.exited


def shquote(s):
    return "'" + str(s).replace("'", "'\\''") + "'"


# -- Databases --------------------------------------------------------------

def each_db(*kinds):
    """Run this test once per database: SQLite always, plus every server in
    LACE_IT_POSTGRES / LACE_IT_MARIADB / LACE_IT_MYSQL (run.py of the
    integration tests sets them). The test takes (home, db).
    @each_db("postgres", "mysql", ...) limits it to those kinds ("mysql"
    covers MariaDB too)."""
    def mark(fn):
        fn.each_db = set(kinds) or {"sqlite", "postgres", "mariadb", "mysql"}
        if "mysql" in fn.each_db:
            fn.each_db.add("mariadb")
        return fn
    if len(kinds) == 1 and callable(kinds[0]):
        fn, kinds = kinds[0], ()
        return mark(fn)
    return mark


def configured_dbs():
    dbs = [("sqlite", None)]
    for kind, env in (("postgres", "LACE_IT_POSTGRES"), ("mariadb", "LACE_IT_MARIADB"),
                      ("mysql", "LACE_IT_MYSQL")):
        if os.environ.get(env):
            dbs.append((kind, os.environ[env]))
    return dbs


class Db:
    """A database the TUI is pointed at. Setup and checks go through lace's
    own CLI query mode (lace -q), so no Python database drivers are needed.
    Each test drops and creates its own tables."""

    def __init__(self, home, kind, url=None):
        self.home, self.kind = home, kind
        self.url = url or "sqlite://" + home.file("db.sqlite") + "?create=1"
        self.mysql = kind in ("mysql", "mariadb")
        self.pg = kind == "postgres"

    def sql(self, sqlite, pg, mysql):
        """This database's dialect of a statement"""
        return pg if self.pg else mysql if self.mysql else sqlite

    def _run(self, sql):
        env = dict(os.environ, HOME=self.home.path,
                   PATH=LACED_DIR + os.pathsep + os.environ.get("PATH", ""))
        r = subprocess.run([LACE, "-q", sql, "-f", "tsv", self.url], capture_output=True,
                           text=True, timeout=120, env=env)
        if r.returncode != 0:
            raise TuiError(f"lace -q failed: {r.stderr.strip()}\n  {sql}")
        return r.stdout

    def exec(self, sql):
        self._run(sql)

    def rows(self, sql):
        """Rows as tuples of strings (NULL is "NULL"; tsv escapes \\t \\n \\r \\\\)"""
        lines = self._run(sql).split("\n")
        out = []
        for line in lines[1:]:  # past the header
            if line == "":
                break
            out.append(tuple(line.split("\t")))
        return out

    def scalar(self, sql):
        return self.rows(sql)[0][0]

    def series(self, table, n):
        """INSERT n rows (id, name, price) into table: id 1..n, name 'name<id>'"""
        if self.pg:
            self.exec(f"INSERT INTO {table} (id, name, price) SELECT g, 'name' || g, "
                      f"g % 97 FROM generate_series(1, {n}) g")
        elif self.mysql:
            cap = ("SET SESSION max_recursive_iterations = 1000000; " if self.kind == "mariadb"
                   else "SET SESSION cte_max_recursion_depth = 1000000; ")
            self.exec(cap + f"INSERT INTO {table} (id, name, price) WITH RECURSIVE "
                      f"s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < {n}) "
                      f"SELECT n, CONCAT('name', n), n % 97 FROM s")
        else:
            self.exec(f"INSERT INTO {table} (id, name, price) WITH RECURSIVE "
                      f"s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < {n}) "
                      f"SELECT n, 'name' || n, n % 97 FROM s")

