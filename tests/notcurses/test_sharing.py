"""One database connection shared by the tabs of several workspaces"""
import importlib.util
import os

from harness import sqlite_db
from nc import Nc
from test_misc import save_connection

# test_table.py of this directory, by path: tests/tui has a module of the same name
_spec = importlib.util.spec_from_file_location(
    "nc_test_table", os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_table.py"))
_tt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_tt)
make_db = _tt.make_db


def test_tx_follows_connection(home):
    db = make_db(home, rows=5)
    save_connection(home, db)
    with Nc(home, ["-s"]) as t:
        t.wait("testdb"); t.keys("Down", "Enter"); t.wait("big")
        t.keys("p"); t.type("BEGIN"); t.keys("C-r")
        t.wait("[TX]")
        t.keys("C-p"); t.type("new workspace"); t.keys("Enter")
        t.wait("Workspace 2")
        t.keys("C-p"); t.type("testdb"); t.keys("Enter")
        t.wait("Workspace 2 ▸ testdb")
        t.wait("[TX]")                        # the same connection: its transaction shows here too
        t.keys("C-w")                         # another tab holds it: no confirmation, no rollback
        t.wait(lambda s: "testdb" not in s.split("\n")[0])
        assert "roll back" not in t.text()
        t.keys("{")
        t.wait("Workspace ▸ testdb")
        t.wait("[TX]")
        t.keys("Escape", "q")                 # out of the editor first
        t.wait("Quit and roll it back")
        t.keys("y")
        assert t.quit() == 0


def test_same_url_opens_once(home):
    db = make_db(home, rows=5)
    with Nc(home, ["-s", "sqlite://" + db, "sqlite://" + db]) as t:
        t.wait(lambda s: "t.sqlite │ t.sqlite" in s.split("\n")[0] or "t.sqlite 2" in s.split("\n")[0])
        t.keys("F8"); t.wait("Log")
        assert t.text().count("list tables") == 1
        t.keys("Escape")
        t.keys("q"); assert t.quit() == 0


def test_close_asks_only_for_last_holder(home):
    db = make_db(home, rows=5)
    save_connection(home, db)
    with Nc(home, ["-s"]) as t:
        t.wait("testdb"); t.keys("Down", "Enter"); t.wait("big")
        t.keys("p"); t.type("BEGIN"); t.keys("C-r")
        t.wait("[TX]")
        t.keys("C-p"); t.type("new workspace"); t.keys("Enter")
        t.wait("Workspace 2")
        t.keys("C-p"); t.type("testdb"); t.keys("Enter")
        t.wait("Workspace 2 ▸ testdb")
        t.keys("{")                    # workspace 1: its only view closes, the tab lets the connection go
        t.wait("Workspace ▸ testdb")
        t.keys("Escape", "C-w")        # workspace 2 still holds it: no question
        t.wait("Workspace ▸ New connection")
        assert "roll back" not in t.text()
        t.keys("}")
        t.wait("Workspace 2 ▸ testdb")
        t.keys("C-w")                  # the last holder: the transaction is named
        t.wait("roll back")
        t.keys("n")
        t.wait("[TX]")
        t.keys("q"); t.wait("Quit and roll it back"); t.keys("y")
        assert t.quit() == 0


def test_saved_edit_opens_new_target(home):
    """A saved connection open in a tab, then edited and saved elsewhere, opens what it now says"""
    db = make_db(home, rows=5)
    other = sqlite_db(home.file("other.sqlite"), "CREATE TABLE other (id INTEGER PRIMARY KEY);")
    save_connection(home, db)
    with Nc(home, ["-s"]) as t:
        t.wait("testdb"); t.keys("Down", "C-b")          # opened in a background tab
        t.wait(lambda s: "testdb" in s.split("\n")[1])
        t.keys("Tab", "Tab", "Tab")                       # URI -> Name -> Driver -> File
        t.keys("C-u"); t.type(other)
        t.keys("C-s"); t.wait_status("Saved")
        t.keys("Enter")
        t.wait("TABLES")
        t.wait(lambda s: "other" in s and " big" not in s.split("\n", 2)[2])
        t.keys("q")
        assert t.quit() == 0
