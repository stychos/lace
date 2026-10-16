import importlib.util
import os

from nc import Nc
from test_misc import SAVED_ID, save_connection

# test_table.py of this directory, by path: tests/tui has a module of the same name
_spec = importlib.util.spec_from_file_location(
    "nc_test_table", os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_table.py"))
_tt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_tt)
make_db, cursor_id = _tt.make_db, _tt.cursor_id


def test_restore_own_session(home):
    db = make_db(home, rows=500)
    save_connection(home, db)
    with Nc(home, []) as t:
        t.wait("testdb"); t.keys("Down", "Enter"); t.wait("big")
        t.keys("Enter"); t.wait("1 of 500")
        t.keys("Right", "o", "o"); t.wait("↓v")
        t.keys("C-g"); t.type("123"); t.keys("Enter"); t.wait("123 of 500")
        t.keys("p"); t.type("SELECT 5"); t.wait("SELECT 5")
        t.keys("q")
        assert t.quit() == 0
    s = home.read("session.json")
    assert s["version"] == 2
    conn = s["workspaces"][0]["connections"][0]
    assert conn["saved_id"] == SAVED_ID and [x["type"] for x in conn["tabs"]] == ["table", "query"]
    assert conn["tabs"][0]["cursor"][0] == 122
    with Nc(home, []) as t:
        t.wait("SELECT 5")                    # the query view was current
        t.keys("[")
        t.wait("123 of 500"); t.wait("↓v"); t.wait("name378")   # the rows are drawn
        assert cursor_id(t) == 500 - 123 + 1
        t.keys("q"); assert t.quit() == 0


def test_restore_ncurses_session(home):
    """A version-1 file the ncurses TUI wrote restores here"""
    db = make_db(home, rows=50)
    save_connection(home, db)
    home.write("session.json", {"settings": {}, "current_workspace": 0, "workspaces": [{
        "name": "from ncurses", "current_tab": 0,
        "tabs": [{"type": "TABLE", "connection_id": SAVED_ID, "table_name": "big",
                  "sort": [{"column": "v", "direction": 2}], "ui": {}}]}]})
    with Nc(home, []) as t:
        t.wait("from ncurses ▸")
        t.wait("↓v"); t.wait("1 of 50"); t.wait("name50")
        assert cursor_id(t) == 50
        t.keys("q"); assert t.quit() == 0


def test_restore_missing_db_is_skipped(home):
    db = make_db(home, rows=5)
    save_connection(home, db)
    home.write("session.json", {"version": 2, "current": 0, "workspaces": [{"name": "W", "current": 0, "connections": [
        {"connstr": "sqlite:///nonexistent/x.db", "name": "gone", "sidebar": True, "current": 0, "tabs": [{"type": "table", "table": "t"}]},
        {"saved_id": SAVED_ID, "name": "testdb", "sidebar": True, "current": 0, "tabs": [{"type": "table", "table": "big"}]}]}]})
    with Nc(home, []) as t:
        t.wait("1 of 5")                       # the second connection restored
        t.keys("F8"); t.wait("gone")           # the failure is in the log
        t.keys("Escape")
        t.keys("q"); assert t.quit() == 0


def test_restore_asks_password(home):
    home.write("connections.json", {"root": {"type": "folder", "name": "Connections", "expanded": True,
        "children": [{"type": "connection", "id": SAVED_ID, "name": "pg", "driver": "postgres", "host": "127.0.0.1",
                      "port": 1, "database": "d", "user": "u", "password": "", "save_password": False}]}})
    home.write("session.json", {"version": 2, "current": 0, "workspaces": [{"name": "W", "current": 0, "connections": [
        {"saved_id": SAVED_ID, "name": "pg", "sidebar": True, "current": 0, "tabs": []}]}]})
    with Nc(home, []) as t:
        t.wait_status("refused", timeout=20)   # port 1: refused, no prompt (not an auth error); the app is usable
        t.keys("q"); assert t.quit() == 0


def test_quit_asks_with_transaction(home):
    db = make_db(home, rows=5)
    with Nc(home, ["-s", "sqlite://" + db]) as t:
        t.wait("big"); t.keys("p"); t.type("BEGIN"); t.keys("C-r")
        t.wait("[TX]")
        t.keys("q")
        t.wait("transaction")                  # the confirmation names it
        t.keys("Escape")
        t.wait("[TX]")
        t.keys("q", "y")
        assert t.quit() == 0


def test_saved_workspace_round_trip(home):
    db = make_db(home, rows=5)
    save_connection(home, db)
    with Nc(home, ["-s"]) as t:
        t.wait("testdb"); t.keys("Down", "Enter"); t.wait("big"); t.keys("Enter"); t.wait("1 of 5")
        t.keys("C-p"); t.type("save workspace"); t.keys("Enter")
        t.wait("Name"); t.type("mine"); t.keys("Enter")
        t.wait_status("saved")
        t.keys("q"); assert t.quit() == 0
    ws = home.read("workspaces.json")["workspaces"]
    assert ws[0]["name"] == "mine" and "connections" in ws[0]["workspace"]
    with Nc(home, ["-s"]) as t:
        t.wait("Lace")                         # the first screen is drawn (input before it is dropped)
        t.keys("C-p"); t.type("open saved workspace mine"); t.keys("Enter")
        t.wait("mine ▸"); t.wait("1 of 5")
        t.keys("q"); assert t.quit() == 0


def test_no_session_saved_when_restore_is_off(home):
    cfg = home.read("config.json")
    cfg["general"]["restore_session"] = False
    home.write("config.json", cfg)
    db = make_db(home, rows=5)
    with Nc(home, ["sqlite://" + db]) as t:
        t.wait("big")
        t.keys("q"); assert t.quit() == 0
    assert home.read("session.json") is None    # as the ncurses TUI: saved only when restored
