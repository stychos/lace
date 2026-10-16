"""The Workspaces window: layout at common terminal sizes, open and saved
workspaces, the recent list, saving (at once, never over a file that did
not load), the connection form, moving items, and connecting with a
password prompt"""

import json
import os
import urllib.parse

from harness import Tui, TuiError, each_db
from test_table import make_db

PLACEHOLDER = "search, or type a URL"


def conns_path(home):
    return os.path.join(home.cfg_dir, "connections.json")


def saved(home):
    with open(conns_path(home)) as f:
        return json.load(f)


def write_conns(home, children):
    with open(conns_path(home), "w") as f:
        json.dump({"root": {"type": "folder", "name": "Connections",
                            "expanded": True, "children": children}}, f)


def sqlite_conn(name, path, cid="11111111-0000-4000-8000-000000000001"):
    return {"type": "connection", "id": cid, "name": name, "driver": "sqlite",
            "host": "", "port": 0, "database": path, "user": "", "password": "",
            "save_password": False}


def open_dialog(home, cols=80, rows=24):
    t = Tui(home, [], cols=cols, rows=rows)
    t.wait(" Workspaces ")
    return t


def test_dialog_at_80x24(home):
    """The most common terminal: the window fills it, the form opens in it"""
    with open_dialog(home) as t:
        t.wait("CONNECTIONS")
        t.keys("Tab", "n")  # From the input to the list, then a new connection
        t.wait("New Connection")
        t.wait("[ Save ]")
        t.keys("Escape")
        t.wait(lambda s: "New Connection" not in t.text())


def test_new_connection_saved_at_once(home):
    """The form's fields (SSH included) reach connections.json before the
    window closes; a bad port is refused in the form"""
    with open_dialog(home) as t:
        t.keys("Tab", "n")
        t.wait("New Connection")
        t.type("Prod")
        t.keys("Tab", "Tab")
        t.type("db.internal")
        t.keys("Tab")
        t.type("99999")
        t.keys("Tab", "Tab", "Tab", "Tab", "Tab", "Space")  # SSH tunnel on
        t.wait("SSH host:")
        t.keys("Enter")
        t.wait("Port: 1 to 65535")
        t.keys("BTab", "BTab", "BTab", "BTab", "BTab", *["BSpace"] * 5)
        t.type("5432")
        t.keys("Tab")
        t.type("app")
        t.keys("Tab", "Tab", "Tab", "Tab", "Tab")
        t.type("bastion")
        t.keys("Enter")
        t.wait("Connection created")
        c = saved(home)["root"]["children"][0]  # The window is still open
        assert (c["name"], c["host"], c["port"], c["database"], c["ssh"],
                c["ssh_host"]) == ("Prod", "db.internal", 5432, "app", True, "bastion")
        assert c["password"] == ""


def test_unreadable_file_never_overwritten(home):
    """A connections.json that does not load is reported and left alone,
    whatever is done in the window"""
    with open(conns_path(home), "w") as f:
        f.write('{"root": {"type": "connection", "id": "x"}}')
    before = open(conns_path(home)).read()
    with open_dialog(home) as t:
        t.wait("connections.json")
        t.keys("Tab", "N")
        t.wait("New Folder")
        t.type("x")
        t.keys("Enter", "Escape")
        assert t.quit() == 0
    assert open(conns_path(home)).read() == before


def test_unknown_items_skipped(home):
    """An item of an unknown kind (a newer lace) does not hide the rest"""
    db = make_db(home, rows=3)
    write_conns(home, [{"type": "bookmark", "x": 1}, sqlite_conn("Mine", db)])
    with open_dialog(home) as t:
        t.wait("Mine")
        assert "connections.json" not in t.text()


def test_move_cancelled_with_escape(home):
    """Esc during a move cancels the move, not the window"""
    db = make_db(home, rows=3)
    write_conns(home, [sqlite_conn("First", db),
                       {"type": "folder", "name": "Folder", "expanded": True,
                        "children": []}])
    with open_dialog(home) as t:
        t.wait("First")
        t.keys("Space")
        t.wait("Moving")
        t.keys("Escape")
        t.wait("Move cancelled")
        assert "Moving" not in t.text() and " Workspaces " in t.text()


def test_move_into_folder(home):
    """Space, Down (into the open folder), Space drops it there"""
    db = make_db(home, rows=3)
    write_conns(home, [sqlite_conn("First", db),
                       {"type": "folder", "name": "Folder", "expanded": True,
                        "children": []}])
    with open_dialog(home) as t:
        t.wait("First")
        t.keys("Space", "Down")
        t.wait("to Folder")
        t.keys("Space")
        t.wait("Moved")
    root = saved(home)["root"]["children"]
    assert [c["name"] for c in root] == ["Folder"]
    assert root[0]["children"][0]["name"] == "First"


def test_ctrl_s_saves_url(home):
    """Ctrl+S is a key (flow control is off): it saves the typed URL"""
    db = make_db(home, rows=3)
    with open_dialog(home) as t:  # Nothing to pick: the input has focus
        t.type("sqlite://" + db)
        t.wait("Connect to")
        t.keys("C-s")
        t.wait("Save Connection")
        t.keys("Enter")
        t.wait("Connection saved")
    assert saved(home)["root"]["children"][0]["database"] == db


def test_connect_saved_and_resize(home):
    """Enter on a saved connection opens it; the window follows a resize,
    the details pane folding away on a narrow terminal"""
    db = make_db(home, rows=3)
    write_conns(home, [sqlite_conn("Big", db)])
    with open_dialog(home, cols=120, rows=35) as t:
        t.wait("Saved connection")  # The details pane
        t.resize(70, 20)
        t.wait("i details")
        t.keys("i")
        t.wait("Saved connection")
        t.keys("i")
        t.resize(120, 35)
        t.wait("Saved connection")
        t.keys("Enter")
        t.wait("Tables")
        assert t.quit() == 0


@each_db("postgres", "mysql")
def test_password_prompt(home, db):
    """A URL without the password: the server refuses, the window asks, a
    wrong answer asks again, the right one connects; the URL is then a
    recent connection, without any password"""
    u = urllib.parse.urlsplit(db.url)
    password = urllib.parse.unquote(u.password or "")
    if not password:
        raise TuiError("the test server URL has no password")
    netloc = f"{u.username}@{u.hostname}:{u.port}"
    url = urllib.parse.urlunsplit((u.scheme, netloc, u.path, u.query, ""))
    with open_dialog(home) as t:
        t.type(url)
        t.keys("Enter")
        t.wait("Password required", timeout=20)
        t.type("wrong-" + password)
        t.keys("Enter")
        t.wait("Wrong password? Try again", timeout=20)
        t.type(password)
        t.keys("Enter")
        t.wait("Tables", timeout=20)
        assert t.quit() == 0
    recent = home.read("recent.json")["recent"]
    assert len(recent) == 1 and password not in json.dumps(recent)


def two_connections(home):
    a = make_db(home, rows=3)
    b = sqlite_file(home, "b.sqlite", "tb")
    write_conns(home, [sqlite_conn("Alpha db", a),
                       {"type": "folder", "name": "Work", "expanded": False,
                        "children": [sqlite_conn("Beta db", b,
                                                 "11111111-0000-4000-8000-000000000002")]}])


def sqlite_file(home, name, table):
    import sqlite3
    path = home.file(name)
    with sqlite3.connect(path) as c:
        c.execute(f"CREATE TABLE {table} (id INTEGER PRIMARY KEY)")
    return path


def test_search(home):
    """/ searches, inside closed folders too; Tab keeps the search and goes
    to the list, Esc clears it (and only then closes the window)"""
    two_connections(home)
    with open_dialog(home, cols=100, rows=30) as t:
        t.wait("Alpha db")
        t.keys("/")
        t.type("bet")
        t.wait("Beta db")
        assert "Alpha db" not in t.text()
        t.keys("Tab")
        t.wait("Saved connection")  # Beta db is selected: its details show
        t.keys("Escape")
        t.wait("Alpha db")
        assert " Workspaces " in t.text()
        t.keys("/")
        t.type("zzz")
        t.wait("(nothing matches)")


def test_mouse(home):
    """A click selects, a footer hint runs, a double click opens a folder
    and connects"""
    two_connections(home)
    with open_dialog(home, cols=100, rows=30) as t:
        t.wait("Work")
        t.keys("Down")  # Away from Alpha db, so the click is what selects it
        x, y = t.find("Alpha db")
        t.click(x, y)
        t.wait("Ctrl+T test")
        bx, by = t.find("Ctrl+T test")
        t.click(bx + 1, by)
        t.wait("Connection works")
        x, y = t.find("Work (1)")
        t.click(x, y, double=True)
        t.wait("Beta db")
        x, y = t.find("Beta db")
        t.click(x, y, double=True)
        t.wait("tb")  # Its table, in the sidebar


def test_workspaces_listed_and_switched(home):
    """Open workspaces show in the window (with the connection they use);
    Enter on one switches to it, t opens a connection as a tab"""
    two_connections(home)
    with open_dialog(home, cols=100, rows=30) as t:
        t.wait("Alpha db")
        t.keys("Enter")
        t.wait("Tables")
        t.keys("w")
        t.wait("OPEN")
        t.wait("open in 1")
        t.keys("/")
        t.type("beta")
        t.keys("Tab", "Enter")  # Beta db in a second workspace
        t.wait("tb")
        t.keys("w")
        t.wait("2  Beta db")
        x, y = t.find("1  Alpha db")
        t.click(x, y, double=True)
        t.wait("big")
        t.keys("w")
        t.wait("2  Beta db")
        t.keys("/")
        t.type("beta")
        t.keys("Down", "Down", "Tab")  # The saved Beta db, below the workspace
        t.wait("t tab here")
        t.keys("t")  # As a tab of workspace 1
        t.wait("tb")
        t.keys("w")
        t.wait("2 tabs")
        assert t.quit() == 0


def test_saved_workspace(home):
    """s saves a workspace's tabs under a name; closed, it opens again from
    Saved workspaces with the same table"""
    two_connections(home)
    with open_dialog(home, cols=100, rows=30) as t:
        t.wait("Alpha db")
        t.keys("Enter")
        t.wait("Tables")
        t.keys("Enter")  # The first table
        t.wait_status("Row 1/3")
        t.keys("w")
        t.wait("OPEN")
        t.keys("s")
        t.wait("Save Workspace")
        t.keys(*["BSpace"] * 30)
        t.type("Mine")
        t.keys("Enter")
        t.wait('Saved "Mine"')
        layouts = home.read("workspaces.json")["workspaces"]
        assert layouts[0]["name"] == "Mine"
        assert layouts[0]["workspace"]["connections"][0]["tabs"][0]["table"] == "big"
        t.keys("Home", "x")  # Close the open workspace
        t.wait("Close Workspace")
        t.keys("y")
        t.wait("Closed")
        assert "OPEN" not in t.text()
        t.keys("Home", "Enter")  # The saved one
        t.wait_status("Row 1/3")
        assert t.quit() == 0


def test_recent(home):
    """A typed file joins Recent; x forgets it"""
    db = make_db(home, rows=3)
    with open_dialog(home, cols=100, rows=30) as t:
        t.type(db)
        t.wait("Connect to")
        t.keys("Enter")
        t.wait("Tables")
        t.keys("w")
        t.wait("RECENT")
        t.wait("t.sqlite")
        t.keys("End", "x")
        t.wait(lambda s: "RECENT" not in s)
        assert t.quit() == 0
    assert home.read("recent.json")["recent"] == []
