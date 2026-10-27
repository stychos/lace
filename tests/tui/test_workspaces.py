"""The Workspaces window (w/F2): the workspaces manager. Open workspaces
(switch, rename, save the layout, close) and saved layouts (open, rename,
delete); n makes a new workspace with a connection form. Connecting happens
in connection tabs, not here."""

import re

from harness import Tui
from test_table import make_db

SAVED_ID = "11111111-0000-4000-8000-000000000001"


def write_conns(home, db):
    home.write("connections.json", {"root": {
        "type": "folder", "name": "Connections", "expanded": True, "children": [
            {"type": "connection", "id": SAVED_ID, "name": "Alpha db", "driver": "sqlite",
             "host": "", "port": 0, "database": db, "user": "", "password": "",
             "save_password": False}]}})


def two_workspaces(home, t):
    """Alpha db in workspace 1 (its table open) and workspace 2"""
    t.wait("URI")
    t.keys("Down")
    t.wait(lambda s: re.search(r"Name\s+Alpha db", s))
    t.keys("Enter")
    t.wait("Tables")
    t.keys("Enter")                      # the first table
    t.wait_status("Row 1/3")
    t.keys("C-t")
    t.wait("URI")
    t.keys("Down")
    t.wait(lambda s: re.search(r"Name\s+Alpha db", s))
    t.keys("C-n")
    t.wait("W2]")


def test_lists_and_switches(home):
    write_conns(home, make_db(home, rows=3))
    with Tui(home, ["-s"], cols=100, rows=30) as t:
        two_workspaces(home, t)
        t.keys("w")
        t.wait("OPEN")
        t.wait("2  Alpha db")
        x, y = t.find("1  Workspace")         # the first workspace is named
        t.click(x, y, double=True)
        t.wait("W1]")
        assert t.quit() == 0


def test_rename_save_close(home):
    """r renames, s saves the layout, x closes; the saved layout opens again"""
    write_conns(home, make_db(home, rows=3))
    with Tui(home, ["-s"], cols=100, rows=30) as t:
        two_workspaces(home, t)
        t.keys("w")
        t.wait("OPEN")
        t.keys("Home", "r")
        t.wait("Rename Workspace")
        t.keys(*["BSpace"] * 30)
        t.type("First")
        t.keys("Enter")
        t.wait("1  First")
        t.keys("s")
        t.wait("Save Workspace")
        t.keys(*["BSpace"] * 30)
        t.type("Mine")
        t.keys("Enter")
        t.wait('Saved "Mine"')
        layouts = home.read("workspaces.json")["workspaces"]
        assert layouts[0]["name"] == "Mine"
        tabs = [tab for c in layouts[0]["workspace"]["connections"] for tab in c.get("tabs", [])]
        assert tabs and tabs[0]["table"] == "big", layouts
        t.keys("Home", "x")
        t.wait("Close Workspace")
        t.keys("y")
        t.wait("Closed")
        assert "1  Mine" not in t.text()
        t.keys("End", "Enter")               # the saved one
        t.wait_status("Row 1/3")
        assert t.quit() == 0


def test_saved_workspace_delete(home):
    write_conns(home, make_db(home, rows=3))
    with Tui(home, ["-s"], cols=100, rows=30) as t:
        two_workspaces(home, t)
        t.keys("w")
        t.wait("OPEN")
        t.keys("Home", "s")
        t.wait("Save Workspace")
        t.keys(*["BSpace"] * 30)
        t.type("Mine")
        t.keys("Enter")
        t.wait('Saved "Mine"')
        t.keys("End", "x")
        t.wait("Delete the saved workspace")
        t.keys("y")
        t.wait("Deleted")
        assert home.read("workspaces.json")["workspaces"] == []
        assert t.quit() == 0


def test_new_workspace(home):
    """n: a new workspace with a connection form"""
    with Tui(home, ["-s"]) as t:
        t.wait("URI")
        t.keys("w")                # the panel leaves w to the hotkeys
        t.wait("OPEN")
        t.keys("n")
        t.wait("W2]")
        t.wait("URI")
        assert t.quit() == 0


def test_no_connections_section(home):
    """No CONNECTIONS or RECENT here, and a typed URL connects to nothing"""
    db = make_db(home, rows=3)
    write_conns(home, db)
    with Tui(home, ["-s"]) as t:
        t.wait("URI")
        t.keys("w")
        t.wait("OPEN")
        assert "CONNECTIONS" not in t.text() and "RECENT" not in t.text()
        t.keys("/")
        t.type(db)
        t.keys("Enter")
        t.wait("nothing matches")
        assert "Tables" not in t.text()
        assert t.quit() == 0


def test_at_80x24(home):
    with Tui(home, ["-s"], cols=80, rows=24) as t:
        t.wait("URI")
        t.keys("w")
        t.wait(" Workspaces ")
        t.wait("? keys")
        t.keys("Escape")
        t.wait(lambda s: " Workspaces " not in s)
        assert t.quit() == 0
