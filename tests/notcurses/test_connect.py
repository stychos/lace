import socket

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


def test_welcome_lists_saved_and_connects(home):
    db = make_db(home, rows=10)
    save_connection(home, db)
    with Nc(home, ["-s"]) as t:
        t.wait("testdb")            # the saved connection on the welcome screen
        t.keys("Down", "Enter")     # first list row after the input
        t.wait("big")
        t.keys("q")
        assert t.quit() == 0


def test_typed_url_and_recent(home):
    db = make_db(home, rows=10)
    with Nc(home, ["-s"]) as t:
        t.wait("palette")           # input before the first draw is discarded
        t.type("sqlite://" + db)
        t.keys("Enter")
        t.wait("big")
        t.keys("C-w")               # close the only view's tab: disconnect
        t.wait("RECENT")
        t.wait("t.sqlite")
        t.keys("q")
        assert t.quit() == 0
    assert any("t.sqlite" in e["url"] for e in home.read("recent.json")["recent"])


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
        t.wait("t.sqlite")
        t.keys("C-PageDown")        # second tab
        t.wait("t.sqlite 2")        # a repeated name gets its index in the header
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


def test_new_connection_form_saves(home):
    db = make_db(home, rows=3)
    with Nc(home, ["-s"]) as t:
        t.wait("palette")
        t.keys("Escape")            # the input gives up the keyboard: letters are keys again
        t.keys("n")
        t.wait("New connection")
        t.type("mine")
        t.keys("Tab", "Left")       # driver: postgres -> sqlite
        t.wait("sqlite")
        t.keys("Tab")               # the file field
        t.type(db)
        t.keys("Enter")
        t.wait("SAVED CONNECTIONS")
        t.wait("mine")
        t.keys("Down", "Enter")
        t.wait("big")
        t.keys("q")
        assert t.quit() == 0
    item = home.read("connections.json")["root"]["children"][0]
    assert item["name"] == "mine" and item["driver"] == "sqlite" and item["database"] == db


def test_form_rejects_a_bad_port(home):
    with Nc(home, ["-s"]) as t:
        t.wait("palette")
        t.keys("Escape", "n")
        t.wait("New connection")
        t.type("srv")
        t.keys("Tab", "Tab", "Tab")  # name -> driver -> host -> port
        t.type("99999")
        t.keys("Enter")
        t.wait("1 to 65535")
        t.keys("Escape")
        t.wait("palette")
        t.keys("q")
        assert t.quit() == 0
    assert home.read("connections.json") is None or not home.read("connections.json")["root"]["children"]


def test_connect_picker_opens_a_tab(home):
    db = make_db(home, rows=3)
    save_connection(home, db)
    with Nc(home, ["-s", "sqlite://" + db]) as t:
        t.wait("big")
        t.keys("w")
        t.wait("Connect")
        t.wait("testdb")
        t.keys("Enter")             # the picker opens on the list: a new tab
        t.wait("Workspace ▸ testdb")
        t.wait("t.sqlite")          # the first tab is still there
        t.keys("C-w")               # closes the current tab
        t.wait("Workspace ▸ t.sqlite")
        t.keys("q")
        assert t.quit() == 0
