import socket

from harness import sqlite_db
from nc import Nc
from test_misc import SAVED_ID, save_connection
from test_table import make_db


def test_connect_from_command_line(home):
    db = make_db(home, rows=10)
    with Nc(home, ["-s", "sqlite://" + db]) as t:
        t.wait("t.sqlite")          # header: connection name
        t.wait("TABLES")            # the sidebar opens on a fresh connection
        t.wait("big")
        t.keys("q")
        assert t.quit() == 0


def first_line(s):
    return s.split("\n")[0]


def save_two(home):
    home.write("connections.json", {"root": {"type": "folder", "name": "Connections", "expanded": True,
        "children": [
            {"type": "connection", "id": SAVED_ID, "name": "a", "driver": "postgres", "host": "ha",
             "port": 5432, "database": "d", "user": "u", "password": "", "save_password": False},
            {"type": "connection", "id": "22222222-2222-3333-4444-555555555555", "name": "b",
             "driver": "postgres", "host": "hb", "port": 5432, "database": "d", "user": "u",
             "password": "", "save_password": False}]}})


def test_start_shows_form(home):
    db = make_db(home, rows=10)
    save_connection(home, db)
    with Nc(home, ["-s"]) as t:
        t.wait("SAVED CONNECTIONS")
        t.wait("URI")
        t.wait(lambda s: first_line(s).startswith(" Workspace ▸ New connection"))
        t.keys("Down")                 # highlights testdb and fills the form
        t.wait("sqlite://")
        t.wait(lambda s: first_line(s).startswith(" Workspace ▸ testdb"))
        t.keys("q")
        assert t.quit() == 0


def test_form_opens_here(home):
    db = make_db(home, rows=10)
    save_connection(home, db)
    with Nc(home, ["-s"]) as t:
        t.wait("testdb"); t.keys("Down", "Enter")
        t.wait("big")
        t.wait("TABLES")
        assert "New connection" not in t.text()   # the form became the connection
        t.keys("q")
        assert t.quit() == 0


def test_typed_url_opens_and_goes_to_recent(home):
    db = make_db(home, rows=10)
    with Nc(home, ["-s"]) as t:
        t.wait("URI")
        t.type("sqlite://" + db)
        t.keys("Enter")
        t.wait("big")
        t.keys("C-w")                  # the tab has no view: it turns back into a form
        t.wait("New connection")
        t.wait("RECENT")
        t.wait("t.sqlite")
        t.keys("Escape", "q")
        assert t.quit() == 0
    assert any("t.sqlite" in e["url"] for e in home.read("recent.json")["recent"])


def test_background_and_new_workspace(home):
    db = make_db(home, rows=10)
    save_connection(home, db)
    with Nc(home, ["-s"]) as t:
        t.wait("testdb"); t.keys("Down", "C-b")
        t.wait(lambda s: "testdb" in s.split("\n")[1] and first_line(s).startswith(" Workspace ▸ testdb │"))
        t.keys("C-n")
        t.wait(lambda s: first_line(s).startswith(" Workspace 2 ▸ testdb"))
        t.keys("{")
        t.wait(lambda s: first_line(s).startswith(" Workspace ▸ testdb │"))
        t.keys("Escape", "q")
        assert t.quit() == 0


def test_to_workspace_picks(home):
    db = make_db(home, rows=10)
    save_connection(home, db)
    with Nc(home, ["-s"]) as t:
        t.wait("testdb")
        t.keys("C-p"); t.type("new workspace"); t.keys("Enter")
        t.wait(lambda s: first_line(s).startswith(" Workspace 2 ▸ New connection"))
        t.keys("Down", "C-o")
        t.wait("Open in workspace")
        t.keys("Enter")                # the only other one: Workspace
        t.wait(lambda s: first_line(s).startswith(" Workspace ▸ testdb"))
        t.keys("q")
        assert t.quit() == 0


def test_dirty_form_asks_before_refill(home):
    save_two(home)
    with Nc(home, ["-s"]) as t:
        t.wait("SAVED CONNECTIONS")
        t.keys("Down")                 # a: fills the form
        t.wait("@ha:5432")
        t.keys("Tab", "Tab", "Tab", "Tab", "Tab", "Tab", "Tab")   # URI -> Name, Driver, Host, Port, Database, User, Password
        t.type("pw")
        t.keys("BTab", "BTab", "BTab", "BTab", "BTab", "BTab", "BTab")   # back to the URI line
        t.wait("u:•••••@ha")
        t.keys("Down")
        t.wait("Drop the changes")
        t.keys("n")
        t.wait("u:•••••@ha")           # still a, password kept
        t.keys("Down")
        t.wait("Drop the changes")
        t.keys("y")
        t.wait("@hb:5432")
        assert "•" not in t.text()
        t.keys("Escape", "q")
        assert t.quit() == 0


def test_save_and_save_as_new(home):
    db = make_db(home, rows=3)
    with Nc(home, ["-s"]) as t:
        t.wait("URI")
        t.type("sqlite://" + db)
        t.keys("Tab")                  # the Name field
        t.type("mine")
        t.keys("C-s")
        t.wait("mine")
        t.wait(lambda s: first_line(s).startswith(" Workspace ▸ mine"))
        t.keys("C-y")
        t.wait_status("Saved")
        t.keys("Escape", "q")
        assert t.quit() == 0
    kids = home.read("connections.json")["root"]["children"]
    assert [k["name"] for k in kids] == ["mine", "mine"] and kids[0]["id"] != kids[1]["id"]
    assert kids[0]["driver"] == "sqlite" and kids[0]["database"] == db


def test_test_button_reports_on_form(home):
    db = make_db(home, rows=3)
    save_connection(home, db)
    with Nc(home, ["-s"]) as t:
        t.wait("testdb"); t.keys("Down", "C-t")
        t.wait("Connection OK")
        t.keys("Escape", "q")
        assert t.quit() == 0


def test_uri_masks_password(home):
    with Nc(home, ["-s"]) as t:
        t.wait("URI")
        t.type("postgres://u:secret@127.0.0.1:1/d")
        t.keys("Tab")                  # the line is rebuilt from the fields
        t.wait("u:•••••@127.0.0.1")
        assert "secret" not in t.text()
        t.keys("Escape", "q")
        assert t.quit() == 0


def test_new_connection_key(home):
    db = make_db(home, rows=3)
    with Nc(home, ["-s", "sqlite://" + db]) as t:
        t.wait("big"); t.keys("Enter"); t.wait("1 of 3")
        t.keys("n")
        t.wait(lambda s: first_line(s).startswith(" Workspace ▸ New connection"))
        t.wait(lambda s: s.split("\n")[1].index("big") < s.split("\n")[1].index("New connection"))
        t.keys("q")
        assert t.quit() == 0


def test_form_rejects_a_bad_port(home):
    with Nc(home, ["-s"]) as t:
        t.wait("URI")
        t.keys("Tab", "Tab", "Tab", "Tab")   # URI -> name -> driver -> host -> port
        t.type("99999")
        t.keys("C-s")
        t.wait("1 to 65535")
        t.keys("Escape", "q")
        assert t.quit() == 0
    assert home.read("connections.json") is None or not home.read("connections.json")["root"]["children"]


def test_password_prompt_on_refusal(home):
    """A saved connection that cannot be reached reports why instead of hanging"""
    home.write("connections.json", {"root": {"type": "folder", "name": "Connections", "expanded": True,
        "children": [{"type": "connection", "id": SAVED_ID, "name": "pg", "driver": "postgres",
                      "host": "127.0.0.1", "port": 1, "database": "d", "user": "u", "password": "",
                      "save_password": False}]}})
    with Nc(home, ["-s"]) as t:
        t.wait("pg")
        t.keys("Down", "Enter")
        t.wait_status("refused", timeout=20)   # nothing listens on port 1: an error, not a hang
        t.keys("q")
        assert t.quit() == 0


def test_connection_tabs_and_workspaces(home):
    db = make_db(home, rows=10)
    with Nc(home, ["-s", "sqlite://" + db, "sqlite://" + db]) as t:
        t.wait("t.sqlite 2")        # a repeated name gets its index in the header
        t.keys("M-1")               # Alt+1: the strip's first cell, the first tab (without views, a cell of its own)
        t.wait(lambda s: s.split("\n")[0].startswith(" Workspace ▸ t.sqlite │ t.sqlite 2"))
        t.keys("}")                 # next workspace: none yet -> message
        t.wait_status("one workspace")
        t.keys("C-p"); t.type("new work"); t.keys("Enter")
        t.wait("Workspace 2")
        t.keys("{")
        t.wait("Workspace ▸")
        t.keys("q")
        assert t.quit() == 0


def test_connecting_overlay_and_cancel(home):
    """A server that accepts and never answers: the progress overlay shows,
    Esc cancels it, and quitting does not wait for the stuck attempt"""
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(5)
    port = srv.getsockname()[1]
    home.write("connections.json", {"root": {"type": "folder", "name": "Connections", "expanded": True,
        "children": [{"type": "connection", "id": SAVED_ID, "name": "mute", "driver": "postgres",
                      "host": "127.0.0.1", "port": port, "database": "d", "user": "u", "password": "",
                      "save_password": False}]}})
    try:
        with Nc(home, ["-s"]) as t:
            t.wait("mute")
            t.keys("Down", "Enter")
            t.wait("Connecting to")
            t.keys("Escape")
            t.wait_status("cancelled")
            t.keys("q")
            assert t.quit(timeout=8) == 0
    finally:
        srv.close()


def set_general(home, **kw):
    cfg = home.read("config.json") or {"general": {}}
    cfg.setdefault("general", {}).update(kw)
    home.write("config.json", cfg)


def test_last_tab_rules(home):
    db = make_db(home, rows=10)
    with Nc(home, ["-s", "sqlite://" + db]) as t:
        t.wait("big"); t.keys("Enter"); t.wait("1 of 10")
        t.keys("C-w")                  # the last view: the tab turns into a form (keep_connection_tab)
        t.wait(lambda s: first_line(s).startswith(" Workspace ▸ New connection"))
        t.keys("C-w")                  # the form, the only tab of the only workspace: lace quits
        assert t.quit() == 0
    set_general(home, on_last_workspace_close="new")
    with Nc(home, ["-s", "sqlite://" + db]) as t:
        t.wait("big"); t.keys("Enter"); t.wait("1 of 10")
        t.keys("C-w")
        t.wait(lambda s: first_line(s).startswith(" Workspace ▸ New connection"))
        t.keys("C-p"); t.type("new workspace"); t.keys("Enter")
        t.wait(lambda s: first_line(s).startswith(" Workspace 2 ▸ New connection"))
        t.keys("C-w")                  # closes workspace 2: back to the first one
        t.wait(lambda s: first_line(s).startswith(" Workspace ▸ New connection"))
        t.keys("C-w")                  # the last one: a fresh workspace with a form
        t.wait(lambda s: first_line(s).startswith(" Workspace ▸ New connection"))
        t.wait("URI")
        t.keys("q")
        assert t.quit() == 0


def test_last_tab_asks_to_quit_when_configured(home):
    set_general(home, quit_confirmation=True)
    with Nc(home, ["-s"]) as t:
        t.wait("URI")
        t.keys("C-w")
        t.wait("Quit lace?")
        t.keys("Escape")               # no: everything stays
        t.wait(lambda s: first_line(s).startswith(" Workspace ▸ New connection"))
        t.keys("C-w"); t.wait("Quit lace?"); t.keys("y")
        assert t.quit() == 0


def test_keep_connection_tab_off(home):
    set_general(home, keep_connection_tab=False)
    db = make_db(home, rows=10)
    db2 = sqlite_db(home.file("u.sqlite"), "CREATE TABLE other (id INTEGER PRIMARY KEY);")
    with Nc(home, ["-s", "sqlite://" + db, "sqlite://" + db2]) as t:
        t.wait("other"); t.keys("Enter")
        t.wait(lambda s: "u.sqlite ▸ other" in first_line(s))
        t.keys("C-w")                  # the last view closes the tab itself
        t.wait(lambda s: first_line(s).startswith(" Workspace ▸ t.sqlite") and "u.sqlite" not in first_line(s))
        assert "New connection" not in t.text()
        t.keys("q")
        assert t.quit() == 0


def test_auto_open_first_table(home):
    set_general(home, auto_open_first_table=True)
    db = make_db(home, rows=10)
    with Nc(home, ["-s", "sqlite://" + db]) as t:
        t.wait("1 of 10")              # the first table opened by itself
        t.keys("q")
        assert t.quit() == 0


def test_tree_management(home):
    db = make_db(home, rows=3)
    save_connection(home, db)
    with Nc(home, ["-s"]) as t:
        t.wait("testdb")
        t.keys("Escape")               # the empty URI line gives the keys to the panel
        t.keys("/"); t.type("nomatch")
        t.wait(lambda s: "testdb" not in s)
        t.keys("Escape")               # the filter goes, the rows come back
        t.wait("testdb")
        t.keys("Down")                 # highlights testdb
        t.wait(lambda s: first_line(s).startswith(" Workspace ▸ testdb"))
        t.keys("N"); t.wait("New folder"); t.type("Work"); t.keys("Enter")
        t.wait("Work")
        assert home.read("connections.json")["root"]["children"][1]["name"] == "Work"
        t.keys("Space")                # move testdb
        t.wait("Move to folder")
        t.keys("Down", "Enter")        # Connections, Work
        t.wait(lambda s: home.read("connections.json")["root"]["children"][0].get("type") == "folder")
        kids = home.read("connections.json")["root"]["children"]
        assert len(kids) == 1 and [k["name"] for k in kids[0]["children"]] == ["testdb"]
        t.keys("d")                    # duplicate, right after it
        t.wait("testdb copy")
        t.keys("r"); t.wait("Rename"); t.keys("C-u"); t.type("other"); t.keys("Enter")
        t.wait("other")
        names = [k["name"] for k in home.read("connections.json")["root"]["children"][0]["children"]]
        assert names == ["other", "testdb copy"]
        t.keys("x"); t.wait("Delete"); t.keys("y")
        t.wait(lambda s: "other" not in s)
        names = [k["name"] for k in home.read("connections.json")["root"]["children"][0]["children"]]
        assert names == ["testdb copy"]
        t.keys("q")
        assert t.quit() == 0


def test_forget_recent(home):
    db = make_db(home, rows=3)
    with Nc(home, ["-s"]) as t:
        t.wait("URI")
        t.type("sqlite://" + db); t.keys("Enter")
        t.wait("big")
        t.keys("C-w")
        t.wait("RECENT")
        t.keys("Escape", "Down")       # the recent row
        t.keys("x")
        t.wait(lambda s: "RECENT" not in s)
        t.keys("q")
        assert t.quit() == 0
    assert not home.read("recent.json")["recent"]


def test_unparsable_uri_does_not_open(home):
    db = make_db(home, rows=3)
    save_connection(home, db)
    with Nc(home, ["-s"]) as t:
        t.wait("testdb"); t.keys("Down")            # the fields hold testdb
        t.wait("sqlite://")
        t.keys("C-u"); t.type(home.file("other.sqlitX"))
        t.keys("Enter")                             # the line does not parse: nothing opens
        t.wait("Not a")
        assert "TABLES" not in t.text()
        t.keys("Escape", "Escape", "q")
        assert t.quit() == 0


def test_uri_applies_when_left(home):
    with Nc(home, ["-s"]) as t:
        t.wait("URI")
        t.type("postgres://u:secret@db1:5432/d")
        t.keys("Tab"); t.wait("db1"); t.keys("BTab")
        t.keys("C-u"); t.type("postgres://ap")      # parses, but is still being typed
        t.wait(lambda s: "Host               db1" in s)   # the fields wait
        t.keys("Tab")                               # leaving the line applies it
        t.wait(lambda s: "Host               ap" in s)
        t.keys("Escape", "q")
        assert t.quit() == 0


def test_auto_open_background_tab(home):
    set_general(home, auto_open_first_table=True)
    db = make_db(home, rows=10)
    save_connection(home, db)
    with Nc(home, ["-s"]) as t:
        t.wait("testdb"); t.keys("Down", "C-b")    # a background tab: not current when its tables load
        t.wait(lambda s: "testdb" in s.split("\n")[1])
        t.keys("Escape", "]")                       # switching to it opens its first table
        t.wait("1 of 10")
        t.keys("q")
        assert t.quit() == 0
