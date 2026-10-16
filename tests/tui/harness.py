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


class Home:
    """A private HOME with a config.json (stdio daemon by default)"""

    def __init__(self, general=None):
        self.path = tempfile.mkdtemp(prefix="lace-tui-")
        self.cfg_dir = config_dir(self.path)
        os.makedirs(self.cfg_dir, exist_ok=True)
        settings = {"daemon_spawn_mode": 1, "restore_session": True,
                    "quit_confirmation": False, "delete_confirmation": True}
        settings.update(general or {})
        self.write("config.json", {"general": settings})

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

    def cleanup(self):
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
        self.tmux("kill-server")

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
        self.wait("Tables")
        self.keys("/")
        self.type(name)
        self.keys("Enter", "Enter")
        return self.wait_status(re.compile(r"Row \d+/\d+"))

    # -- input --------------------------------------------------------------

    def keys(self, *keys, delay=0.05):
        """tmux key names: Enter, Escape, Up, PPage, C-r, BSpace, Tab, BTab, ..."""
        for k in keys:
            self.tmux("send-keys", "-t", "t", k)
            time.sleep(delay)

    def type(self, text):
        self.tmux("send-keys", "-t", "t", "-l", text)
        time.sleep(0.05)

    def click(self, x, y, double=False, wheel=None):
        """Mouse at screen column x, row y (0-based), as an xterm X10 report:
        a left click (two for double), or wheel="up"/"down" """
        def report(b):
            return "\x1b[M" + chr(32 + b) + chr(33 + x) + chr(33 + y)
        if wheel:
            self.type(report(64 if wheel == "up" else 65))
            return
        for _ in range(2 if double else 1):
            self.type(report(0))
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

    def cursor_row_text(self):
        """Text of the row holding the cursor cell, which is drawn black on
        cyan (other cyan backgrounds, like the sidebar filter, are not)"""
        raw = self.tmux("capture-pane", "-t", "t", "-p", "-e", capture=True)
        for line in raw.split("\n"):
            plain = re.sub(r"\x1b\[[0-9;]*m", "", line)
            # Table rows start with a border; the header, tab and status
            # bars (also black on cyan) do not
            if "\x1b[30m\x1b[46m" in line and plain[:1] in ("│", "└", "├", "┌"):
                return plain
        return None

    def cursor_cells(self):
        """The cells of the cursor row (sidebar excluded), stripped"""
        line = self.cursor_row_text()
        if line is None:
            return None
        # With the sidebar shown, its column comes first, closed by │, ┘, ┤
        # or ┐ depending on the row
        m = re.match(r"^[│└├┌][^│┘┤┐]*[│┘┤┐]", line)
        if m:
            line = line[m.end():]
        return [p.strip() for p in line.split("│")]

    def row_number(self):
        m = re.search(r"Row (\d+)/", self.status())
        return int(m.group(1)) if m else None

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
        r = subprocess.run([LACE, "-q", sql, self.url], capture_output=True,
                           text=True, timeout=120, env=env)
        if r.returncode != 0:
            raise TuiError(f"lace -q failed: {r.stderr.strip()}\n  {sql}")
        return r.stdout

    def exec(self, sql):
        self._run(sql)

    def rows(self, sql):
        """Rows as tuples of strings (NULL is "NULL")"""
        lines = self._run(sql).split("\n")
        out = []
        for line in lines[2:]:  # Header and "---" rule
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

