"""The connection form tab: what lace shows at start without a session, the
saved connections and Recent on the left, the URI line and fields on the
right, the four places a connection opens in, and reusing a connection that
is already open"""

import json
import os
import re
import time
import urllib.parse

from harness import Tui, TuiError, each_db
from test_table import make_db


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


def start(home, cols=120, rows=30):
    t = Tui(home, ["-s"], cols=cols, rows=rows)
    t.wait("URI")
    return t


def local(home):
    """One saved SQLite connection, "Local", to a small database"""
    db = make_db(home, rows=3)
    write_conns(home, [sqlite_conn("Local", db)])
    return db


def test_start_shows_form(home):
    """No session and no URL: a connection form, not the Workspaces window"""
    with start(home) as t:
        t.wait("New connection")
        assert " Workspaces " not in t.text()
        assert t.quit() == 0


def test_form_opens_here(home):
    """Highlighting a saved connection fills the form and names the tab;
    Enter opens it in this tab, which then lists the tables"""
    local(home)
    with start(home) as t:
        t.keys("Down")
        t.wait(lambda s: re.search(r"Name\s+Local", s))
        t.keys("Enter")
        t.wait("TABLES")
        t.wait("big")
        assert "New connection" not in t.text()
        assert t.quit() == 0


def test_typed_url_opens_and_goes_to_recent(home):
    """A file path typed into the URI line opens and joins Recent, which the
    next form lists"""
    db = make_db(home, rows=3)
    with start(home) as t:
        t.type(db)
        t.keys("Enter")
        t.wait("TABLES")
        recent = home.read("recent.json")["recent"]
        assert len(recent) == 1 and recent[0]["url"].startswith("sqlite://")
        t.keys("C-t")                       # new_connection: a form after this tab
        t.wait("RECENT")
        t.wait("t.sqlite")
        assert t.quit() == 0


def test_background_and_new_workspace(home):
    """Ctrl+B opens it in a tab after the form, which stays current; Ctrl+N
    opens it in a new workspace"""
    local(home)
    with start(home) as t:
        t.keys("Down")
        t.wait(lambda s: re.search(r"Name\s+Local", s))
        t.keys("C-b")
        # the strip: the form, then the connection's tab (its saved name)
        t.wait(lambda s: len(s.splitlines()) > 1 and s.splitlines()[1].count("Local") == 2)
        assert "URI" in t.text()            # the form is still current
        t.keys("C-n")
        t.wait("TABLES")
        t.wait_workspace("Workspace 2")      # the header: workspace 2
        assert t.quit() == 0


def test_to_workspace_picks(home):
    """Ctrl+O lists the other workspaces; the connection opens at the end of
    the one picked, which becomes current"""
    local(home)
    with start(home) as t:
        t.keys("Down")
        t.wait(lambda s: re.search(r"Name\s+Local", s))
        t.keys("C-o")
        t.wait("No other workspace")
        t.keys("C-n")                       # a second workspace, with the connection
        t.wait("TABLES")
        t.keys("{")                         # back to the first, still a form
        t.wait("URI")
        t.keys("C-o")
        t.wait("Open in workspace")
        t.keys("Enter")
        t.wait("TABLES")
        t.wait_workspace("Workspace 2")
        assert t.quit() == 0


def test_same_connection_opens_once(home):
    """A saved connection already open is used again, not connected twice: a
    transaction opened in one tab shows in the other"""
    local(home)
    with start(home) as t:
        t.keys("Down")
        t.wait(lambda s: re.search(r"Name\s+Local", s))
        t.keys("Enter")
        t.wait("TABLES")
        t.keys("p")
        t.type("BEGIN")
        t.keys("C-r")
        t.wait_status("[TX]")
        t.keys("Escape")
        time.sleep(0.2)                      # Esc and a key at once is Alt+key
        # a query tab's Ctrl+T runs the transaction: the palette opens the
        # form (it does not send Ctrl+T there)
        t.keys("C-p")
        t.wait("Command palette")
        t.type("new connection")
        t.wait("New connection")
        t.keys("Enter")
        t.wait("URI")
        t.keys("Down")
        t.wait("open in 1")
        t.keys("C-b")
        t.keys("Escape")                    # to the panel, then the next tab
        t.wait_status("folder")
        t.keys("]")
        t.wait("TABLES")
        t.wait_status("[TX]")
        assert t.quit() == 0


def test_uri_masks_password(home):
    """A password typed in the URI line goes to the Password field and is
    never shown"""
    with start(home) as t:
        t.type("postgres://app:hunter2@db1:5432/shop")
        t.keys("Tab")
        t.wait(lambda s: re.search(r"Host\s+db1", s))
        t.wait("•••••")
        assert "hunter2" not in t.text()
        t.keys("C-l")
        assert t.quit() == 0


def test_unparsable_uri_does_not_open(home):
    """Text in the URI line that is not a URL or a file blocks Open, even
    with a saved connection highlighted"""
    local(home)
    with start(home) as t:
        t.keys("Down")
        t.wait(lambda s: re.search(r"Name\s+Local", s))
        t.keys("C-u")                       # the line held Local's URI
        t.type("::nonsense")
        t.keys("Enter")
        t.wait("Not a connection URL")
        assert "TABLES" not in t.text()
        t.keys("C-u", "C-l")
        assert t.quit() == 0


def test_form_rejects_a_bad_port(home):
    with start(home) as t:
        t.keys("Tab", "Tab", "Tab", "Tab")  # URI -> name -> driver -> host -> port
        t.type("99999")
        t.keys("C-s")
        t.wait("1 to 65535")
        t.keys("C-l")
        assert t.quit() == 0
    c = home.read("connections.json")
    assert c is None or not c["root"]["children"]


@each_db("postgres", "mysql")
def test_password_prompt(home, db):
    """A URL without the password: the server refuses, lace asks, a wrong
    answer asks again, the right one connects; the URL is then a recent
    connection, without any password"""
    u = urllib.parse.urlsplit(db.url)
    password = urllib.parse.unquote(u.password or "")
    if not password:
        raise TuiError("the test server URL has no password")
    netloc = f"{u.username}@{u.hostname}:{u.port}"
    url = urllib.parse.urlunsplit((u.scheme, netloc, u.path, u.query, ""))
    with start(home) as t:
        t.type(url)
        t.keys("Enter")
        t.wait("Password required", timeout=20)
        t.type("wrong-" + password)
        t.keys("Enter")
        t.wait("Wrong password? Try again", timeout=20)
        t.type(password)
        t.keys("Enter")
        t.wait("TABLES", timeout=20)
        assert t.quit() == 0
    recent = home.read("recent.json")["recent"]
    assert len(recent) == 1 and password not in json.dumps(recent)


def pg_conn(name, host, cid):
    return {"type": "connection", "id": cid, "name": name, "driver": "postgres",
            "host": host, "port": 5432, "database": "shop", "user": "app",
            "password": "", "save_password": False}


def test_dirty_form_asks_before_refill(home):
    """With a password typed and not saved, moving the highlight asks; No
    keeps the highlight and every field, Yes fills the next entry"""
    write_conns(home, [pg_conn("Alpha", "h1", "11111111-0000-4000-8000-00000000000a"),
                       pg_conn("Beta", "h2", "11111111-0000-4000-8000-00000000000b")])
    with start(home) as t:
        t.keys("Down")
        t.wait(lambda s: re.search(r"Name\s+Alpha", s))
        # URI -> name -> driver -> host -> port -> database -> user -> password
        t.keys("Tab", "Tab", "Tab", "Tab", "Tab", "Tab", "Tab")
        t.type("x")
        t.wait(lambda s: re.search(r"Password\s+\*", s))
        t.keys("Escape")                     # back to the URI line
        t.wait_status("background")
        t.keys("Down")                       # the next row
        t.wait("Drop the changes in the form?")
        t.keys("n")
        t.wait(lambda s: "Drop the changes" not in s)
        assert re.search(r"Name\s+Alpha", t.text())
        assert re.search(r"Password\s+\*", t.text())
        t.keys("Down")
        t.wait("Drop the changes in the form?")
        t.keys("y")
        t.wait(lambda s: re.search(r"Name\s+Beta", s))
        assert not re.search(r"Password\s+\*", t.text())
        assert t.quit() == 0


def test_save_and_save_as_new(home):
    """Save writes over the highlighted entry; Save as new adds one"""
    write_conns(home, [pg_conn("Alpha", "h1", "11111111-0000-4000-8000-00000000000a")])
    with start(home) as t:
        t.keys("Down")
        t.wait(lambda s: re.search(r"Name\s+Alpha", s))
        t.keys("Tab", "C-u")
        t.type("A2")
        t.keys("C-s")
        t.wait("Saved A2")
        names = [c["name"] for c in saved(home)["root"]["children"]]
        assert names == ["A2"], names
        t.keys("C-u")
        t.type("A3")
        t.keys("C-y")
        t.wait("Saved A3")
        names = sorted(c["name"] for c in saved(home)["root"]["children"])
        assert names == ["A2", "A3"], names
        assert t.quit() == 0


def test_test_button_reports_on_form(home):
    """Ctrl+T tests what the form shows; a failure is a message, not a crash"""
    db = make_db(home, rows=3)
    write_conns(home, [sqlite_conn("Local", db),
                       sqlite_conn("Gone", home.file("missing.sqlite"),
                                   "11111111-0000-4000-8000-000000000002")])
    with start(home) as t:
        t.keys("Down")
        t.wait(lambda s: re.search(r"Name\s+Local", s))
        t.keys("C-t")
        t.wait("Connection works")
        t.keys("Down")
        t.wait(lambda s: re.search(r"Name\s+Gone", s))
        t.keys("C-t")
        t.wait("failed")
        assert t.quit() == 0


def test_tree_management(home):
    """With the panel focused: N adds a folder, Space moves, r renames, d
    duplicates, x deletes; each change is written at once"""
    local(home)
    with start(home) as t:
        t.keys("Escape")                     # the panel
        t.wait_status("folder")
        t.keys("N")
        t.wait("New Folder")
        t.type("Work")
        t.keys("Enter")
        t.wait("Work")
        root = saved(home)["root"]["children"]
        assert [c["name"] for c in root] == ["Local", "Work"], root
        t.keys("Down")                       # highlight Local
        t.wait(lambda s: re.search(r"Name\s+Local", s))
        t.keys("Space", "Down", "Space")     # into the open folder below it
        t.wait(lambda s: saved(home)["root"]["children"][0]["name"] == "Work")
        t.keys("r")
        t.wait("Rename")
        t.keys("C-u")
        t.type("Home")
        t.keys("Enter")
        t.wait(lambda s: "Home" in json.dumps(saved(home)))
        t.keys("d")
        t.wait(lambda s: "(copy)" in json.dumps(saved(home)))
        t.keys("x")
        t.wait("Delete")
        t.keys("y")
        t.wait(lambda s: json.dumps(saved(home)).count("Home") == 1)
        assert t.quit() == 0


def test_forget_recent(home):
    """x on a Recent row forgets it"""
    db = make_db(home, rows=3)
    with start(home) as t:
        t.type(db)
        t.keys("Enter")
        t.wait("TABLES")
        t.keys("C-t")
        t.wait("RECENT")
        t.keys("Escape")                     # the panel (Esc and a key at once is Alt+key)
        t.wait_status("folder")
        t.keys("Down")                       # the Recent row
        t.wait(lambda s: re.search(r"File\s+\S*t\.sqlite", s))
        t.keys("x")
        t.wait(lambda s: "RECENT" not in s)
        assert home.read("recent.json")["recent"] == []
        assert t.quit() == 0


def test_filter(home):
    """/ filters the panel; Esc clears it"""
    write_conns(home, [pg_conn("Alpha", "h1", "11111111-0000-4000-8000-00000000000a"),
                       pg_conn("Beta", "h2", "11111111-0000-4000-8000-00000000000b")])
    with start(home) as t:
        t.keys("Escape")
        t.wait_status("folder")
        t.keys("/")
        t.type("Be")
        t.wait(lambda s: "Beta" in s and "Alpha" not in s)
        t.keys("Escape")
        t.wait("Alpha")
        assert t.quit() == 0


def test_mouse(home):
    """A click on a panel row highlights it, a double click opens it, a click
    on [Test] tests"""
    local(home)
    with start(home) as t:
        x, y = t.find("Local")
        t.click(x, y)
        t.wait(lambda s: re.search(r"Name\s+Local", s))
        bx, by = t.find("[Test]")
        t.click(bx + 1, by)
        t.wait("Connection works")
        t.click(x, y, double=True)           # the panel row found before
        t.wait("TABLES")
        assert t.quit() == 0


def test_no_query_tab_without_connection(home):
    """p (open query) on a form makes no query tab: there is no connection"""
    with start(home) as t:
        t.keys("Escape")                     # the panel, which leaves p to the hotkeys
        t.wait_status("folder")
        t.keys("p")
        t.wait_status("connection")
        assert "Query" not in t.screen()[1]
        assert t.quit() == 0


def test_filter_keeps_saved_identity(home):
    """After the panel's filter dropped the highlight, a clean form showing a
    saved connection still opens that saved connection (shared, not a URL
    for Recent)"""
    local(home)
    with start(home) as t:
        t.keys("Down")
        t.wait(lambda s: re.search(r"Name\s+Local", s))
        t.keys("Escape")
        t.wait_status("folder")
        t.keys("/")
        t.type("Lo")
        t.keys("Enter")                      # leaves the filter
        t.keys("Enter")                      # opens what the form shows
        t.wait("TABLES")
        r = home.read("recent.json")
        assert not r or r["recent"] == [], r
        assert t.quit() == 0
