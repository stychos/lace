"""Filters, sessions, dialogs, terminal sizes, text, CLI query mode"""

import json
import os
import re
import subprocess
import time

from harness import LACE, LACED_DIR, Tui, sqlite_db
from test_table import cursor_id, make_db

SAVED_ID = "11111111-2222-3333-4444-555555555555"


def save_connection(home, db, name="testdb"):
    home.write("connections.json", {"root": {
        "type": "folder", "name": "Connections", "expanded": True,
        "children": [{"type": "connection", "id": SAVED_ID, "name": name,
                      "driver": "sqlite", "host": "", "port": 0, "database": db,
                      "user": "", "password": "", "save_password": False}]}})


def test_filter(home):
    db = make_db(home, rows=100)
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        t.open_table("big")
        t.keys("Right", "/")  # A filter on column v, cursor on its value
        t.wait("Filters")
        t.keys("Enter")
        t.type("42")
        t.keys("Enter")
        t.wait_header("1 of 1")
        t.keys("X")  # Clear filters
        t.wait_header("1 of 100")


def test_session_restore(home):
    """Quit with a sorted table open at a row; start again: same table,
    same order, same row"""
    db = make_db(home, rows=500)
    save_connection(home, db)
    with Tui(home, []) as t:
        t.wait("testdb")  # No session yet: a connection form lists it
        t.keys("Down", "Enter")
        t.wait("TABLES")
        t.keys("Enter")
        t.wait_header(re.compile(r"1 of 500"))
        t.keys("Right", "o", "o")  # v DESC
        t.wait("\u2193")              # a descending sort
        t.keys("C-g")
        t.type("123")
        t.keys("Enter")
        t.wait_header("123 of 500")
        assert t.quit() == 0
    session = home.read("session.json")
    conn = session["workspaces"][0]["connections"][0]
    tab = conn["tabs"][0]
    assert conn["saved_id"] == SAVED_ID and tab["table"] == "big"
    assert tab["sorts"][0]["column"] == "v"
    with Tui(home, []) as t:
        t.wait_header("123 of 500")
        t.wait("\u2193")              # a descending sort
        assert cursor_id(t) == 500 - 123 + 1


def test_dialogs_open_and_close(home):
    db = make_db(home, rows=5)
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        t.open_table("big")
        # "?" is the hotkey list of the configuration dialog
        for key, marker in [("w", "search workspaces"), (",", "Preferences"),
                            ("?", "Previous tab"), ("i", "History")]:
            t.keys(key)
            t.wait(marker)
            t.keys("Escape")
            t.wait(lambda s, m=marker: m not in s)
        t.keys("s")                     # the Structure sub-view, not a dialog: `a` is the way back
        t.wait("Extra")
        t.keys("a")
        t.wait(lambda s: "Extra" not in s)
        assert t.quit() == 0


def test_tiny_terminal(home):
    db = make_db(home, rows=50)
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        t.open_table("big")
        t.resize(30, 8)
        # Below the minimum there are no windows: the loop must still wait
        # for input, not spin
        before = t.cpu_seconds()
        time.sleep(2)
        assert t.cpu_seconds() - before < 1.0
        t.keys("Down", "Down")
        t.resize(120, 30)
        t.wait_header("3 of 50")
        assert t.quit() == 0


def test_utf8_cells(home):
    db = sqlite_db(home.file("u.sqlite"), """
        CREATE TABLE u (id INTEGER PRIMARY KEY, s TEXT);
        INSERT INTO u VALUES (1, 'héllo wörld'), (2, '日本語テキスト');
    """)
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        t.open_table("u")
        t.wait("héllo wörld")
        t.wait("日本語テキスト")


def test_cli_query_mode(home):
    db = sqlite_db(home.file("c.sqlite"), """
        CREATE TABLE c (id INTEGER PRIMARY KEY, s TEXT);
        INSERT INTO c VALUES (1, 'a'), (2, 'b');
    """)
    def q(*args):
        out = subprocess.run([LACE, "-q", *args, "sqlite://" + db], capture_output=True, text=True,
                             timeout=30, env={"HOME": home.path, "PATH": LACED_DIR + ":/usr/bin:/bin"})
        assert out.returncode == 0, out.stderr
        return out.stdout
    sql = "SELECT id, s FROM c ORDER BY id"
    # The default: an aligned table, numbers right-aligned, a row count
    assert q(sql) == ("+----+---+\n| id | s |\n+----+---+\n|  1 | a |\n|  2 | b |\n"
                      "+----+---+\n2 rows\n"), q(sql)
    assert q(sql, "-f", "csv") == "id,s\n1,a\n2,b\n"
    assert q(sql, "--format=json") == '[\n {"id": 1, "s": "a"},\n {"id": 2, "s": "b"}\n]\n'
    assert q(sql, "-f", "tsv") == "id\ts\n1\ta\n2\tb\n"
    # MySQL's \G at the end, or -f vertical: a block per row
    vertical = ("*************************** 1. row ***************************\nid: 1\n s: a\n"
                "*************************** 2. row ***************************\nid: 2\n s: b\n2 rows\n")
    assert q(sql + "\\G") == vertical
    assert q(sql, "-f", "vertical") == vertical
    # A statement without rows: its count
    assert q("UPDATE c SET s = s") == "2 rows affected\n"
    bad = subprocess.run([LACE, "-q", sql, "-f", "xml", "sqlite://" + db], capture_output=True,
                         text=True, timeout=30, env={"HOME": home.path})
    assert bad.returncode != 0 and "Unknown format 'xml'" in bad.stderr


def test_socket_daemon_spawn(home):
    """Default spawn mode: the TUI starts a detached socket daemon at the
    user's default path; it outlives the TUI and leaves by its idle timeout"""
    import os
    import time
    from harness import Home
    socket_home = Home({"daemon_spawn_mode": 0, "daemon_idle_timeout": 2})
    try:
        db = make_db(socket_home, rows=5)
        sock = (os.path.join(socket_home.path, "Library", "Caches", "laced.sock")
                if os.uname().sysname == "Darwin"
                else os.path.join(socket_home.path, ".cache", "lace", "laced.sock"))
        with Tui(socket_home, ["-s", "sqlite://" + db],
                 env={"XDG_RUNTIME_DIR": ""}) as t:
            t.open_table("big")
            assert os.path.exists(sock), "no daemon socket at " + sock
            assert t.quit() == 0
        # Idle for 2 s after its last client: the daemon removes its socket
        deadline = time.time() + 10
        while os.path.exists(sock) and time.time() < deadline:
            time.sleep(0.2)
        assert not os.path.exists(sock), "daemon still running after idle timeout"
    finally:
        socket_home.cleanup()



# -- Spawn mode "detect" (3): a running daemon, else a private stdio one ----

DETECT_PORT = 7433  # LACE_DEFAULT_PORT: detection only looks there


def _detect_home():
    from harness import Home
    return Home({"daemon_spawn_mode": 3})


def _cache_dir(home):
    return (os.path.join(home.path, "Library", "Caches")
            if os.uname().sysname == "Darwin"
            else os.path.join(home.path, ".cache", "lace"))


def _start_laced(home, *args):
    """A daemon in the given HOME (its default socket and token file live
    there), logging to its own file"""
    env = dict(os.environ, HOME=home.path, XDG_RUNTIME_DIR="",
               LACED_LOG=home.file("shared-laced.log"))
    return subprocess.Popen([os.path.join(LACED_DIR, "laced"), *args], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _wait_file(path, timeout=10.0):
    deadline = time.time() + timeout
    while not os.path.exists(path) and time.time() < deadline:
        time.sleep(0.05)
    assert os.path.exists(path), "no " + path


def _stop(proc):
    proc.terminate()
    proc.wait(timeout=10)


def _port_busy():
    import socket
    probe = socket.socket()
    try:
        return probe.connect_ex(("127.0.0.1", DETECT_PORT)) == 0
    finally:
        probe.close()


def test_detect_uses_unix_daemon(home):
    """A shared daemon on the default Unix socket is used, not a private one"""
    h = _detect_home()
    sock = os.path.join(_cache_dir(h), "laced.sock")
    os.makedirs(_cache_dir(h), mode=0o700, exist_ok=True)
    daemon = _start_laced(h, "--unix", sock)
    try:
        _wait_file(sock)
        db = make_db(h, rows=5)
        with Tui(h, ["-s", "sqlite://" + db], env={"XDG_RUNTIME_DIR": ""}) as t:
            t.open_table("big")
            t.wait_header("1 of 5")
            assert t.child_daemons() == [], "started a private daemon"
            assert t.quit() == 0
        assert daemon.poll() is None, "the shared daemon went away"
    finally:
        _stop(daemon)
        h.cleanup()


def test_detect_uses_tcp_daemon(home):
    """No Unix socket, a TCP daemon on the default port: it is used if it
    passes the token handshake. One that does not (a token we cannot read,
    another user's daemon) is no daemon for us: a private one serves instead,
    and nothing secret went to the listener"""
    if _port_busy():
        print(f"       (port {DETECT_PORT} in use here: skipped)")
        return
    h = _detect_home()
    daemon = _start_laced(h, "--tcp", str(DETECT_PORT))
    try:
        token = os.path.join(_cache_dir(h), f"laced-{DETECT_PORT}.token")
        _wait_file(token)
        db = make_db(h, rows=5)
        with Tui(h, ["-s", "sqlite://" + db], env={"XDG_RUNTIME_DIR": ""}) as t:
            t.open_table("big")
            t.wait_header("1 of 5")
            assert t.child_daemons() == [], "started a private daemon"
            assert t.quit() == 0
        with open(token) as f:
            real = f.read()
        with open(token, "w") as f:
            f.write("0" * 32 + "\n")
        try:
            with Tui(h, ["-s", "sqlite://" + db], env={"XDG_RUNTIME_DIR": ""}) as t:
                t.open_table("big")
                t.wait_header("1 of 5")
                assert len(t.child_daemons()) == 1, "did not fall back to a private daemon"
                assert t.quit() == 0
        finally:
            with open(token, "w") as f:
                f.write(real)
    finally:
        _stop(daemon)
        h.cleanup()


def test_detect_private_daemon(home):
    """Nothing running: a private stdio daemon, which leaves nothing behind"""
    h = _detect_home()
    try:
        db = make_db(h, rows=5)
        with Tui(h, ["-s", "sqlite://" + db], env={"XDG_RUNTIME_DIR": ""}) as t:
            t.open_table("big")
            t.wait_header("1 of 5")
            assert len(t.child_daemons()) == 1, "no private daemon"
            assert t.quit() == 0
        cache = _cache_dir(h)
        left = os.listdir(cache) if os.path.isdir(cache) else []
        assert not [f for f in left if f.startswith("laced")], left
    finally:
        h.cleanup()


def test_detect_again_after_loss(home):
    """The private daemon dies and a shared one has appeared: the next call
    detects again and the TUI carries on with the shared daemon"""
    import signal
    h = _detect_home()
    daemon = None
    try:
        db = make_db(h, rows=5)
        with Tui(h, ["-s", "sqlite://" + db], env={"XDG_RUNTIME_DIR": ""}) as t:
            t.open_table("big")
            t.wait_header("1 of 5")
            private = t.child_daemons()
            assert len(private) == 1, "no private daemon"
            sock = os.path.join(_cache_dir(h), "laced.sock")
            os.makedirs(_cache_dir(h), mode=0o700, exist_ok=True)
            daemon = _start_laced(h, "--unix", sock)
            _wait_file(sock)
            os.kill(private[0], signal.SIGKILL)
            time.sleep(0.3)
            t.keys("r")  # Refresh: the first call after the loss
            t.wait_status("Refreshed")
            assert t.child_daemons() == [], "still on a private daemon"
            t.keys("Down")
            t.wait_header("2 of 5")
            assert t.quit() == 0
    finally:
        if daemon:
            _stop(daemon)
        h.cleanup()
