"""session.json version 2: written nested, read in both shapes"""

import json
import os

from harness import Tui
from test_misc import SAVED_ID, save_connection
from test_table import cursor_id, make_db


def test_session_v2_round_trip(home):
    """Quit with two tabs of one connection, sorted; the file is version 2
    with the nested shape, keeps a foreign section, and restores"""
    db = make_db(home, rows=500)
    save_connection(home, db)
    # Another frontend's section must survive our save
    home.write("session.json", {"version": 2, "workspaces": [], "current": 0,
                                "notcurses": {"theme": "dark"}})
    with Tui(home, ["-s"]) as t:
        t.wait("testdb")
        t.keys("Enter")
        t.wait("Tables")
        t.keys("Enter")
        t.wait_status("Row 1/500")
        t.keys("Right", "o", "o")  # v DESC
        t.wait("▼")
        t.keys("C-g")
        t.type("123")
        t.keys("Enter")
        t.wait_status("Row 123/500")
        t.keys("p")  # a query tab on the same connection
        t.wait("SQL Query")
        assert t.quit() == 0
    s = home.read("session.json")
    assert s["version"] == 2 and s["notcurses"] == {"theme": "dark"}
    ws = s["workspaces"][0]
    conn = ws["connections"][0]
    assert conn["saved_id"] == SAVED_ID
    types = [tab["type"] for tab in conn["tabs"]]
    assert types == ["table", "query"]
    tab = conn["tabs"][0]
    assert tab["table"] == "big" and tab["sorts"] == [{"column": "v", "desc": True}]
    assert tab["order"] == 0 and conn["tabs"][1]["order"] == 1
    assert "settings" not in s and "ncurses" in s
    with Tui(home, []) as t:
        t.wait("SQL Query")  # the current tab (the query) is restored
        t.keys("[")    # back to the table tab
        t.wait_status("Row 123/500")
        t.wait("▼")
        assert cursor_id(t) == 500 - 123 + 1


def test_session_v2_from_gui(home):
    """A nested workspace written by a GUI (no order, a filter with starts)
    restores, and a connstr-only connection reconnects by its string"""
    db = make_db(home, rows=100)
    save_connection(home, db)
    home.write("session.json", {"version": 2, "current": 0, "workspaces": [{
        "name": "G", "current": 0, "connections": [{
            "saved_id": SAVED_ID, "name": "testdb", "sidebar": True, "current": 0,
            "tabs": [{"type": "table", "table": "big", "filters_visible": False,
                      "filters": [{"column": "name", "op": "starts", "value": "name1"}],
                      "sorts": []}]},
            {"connstr": "sqlite://" + db, "name": "by url", "sidebar": True, "current": 0,
             "tabs": [{"type": "query", "text": "SELECT 7"}]}]}]})
    with Tui(home, []) as t:
        # name1, name10..name19, name100: 12 rows
        t.wait_status("Row 1/12")
        t.keys("]")
        t.wait("SELECT 7")


BAD = '{"version": 2, "work'


def test_session_v2_unparsable_kept(home):
    """A session.json that does not parse is never overwritten by a save"""
    db = make_db(home, rows=5)
    save_connection(home, db)
    with Tui(home, ["-s"]) as t:
        t.wait("testdb")
        t.keys("Enter")
        t.wait("Tables")
        t.keys("Enter")
        t.wait_status("Row 1/5")
        with open(home.cfg_dir + "/session.json", "w") as f:
            f.write(BAD)
        t.quit()
    with open(home.cfg_dir + "/session.json") as f:
        assert f.read() == BAD


def test_session_v1_still_loads(home):
    """A version-1 file from an older build restores unchanged"""
    db = make_db(home, rows=50)
    save_connection(home, db)
    home.write("session.json", {"settings": {"sidebar_visible": True}, "current_workspace": 0,
                                "workspaces": [{"name": "", "current_tab": 0, "tabs": [
                                    {"type": "TABLE", "connection_id": SAVED_ID, "table_name": "big",
                                     "sort": [{"column": "v", "direction": 2}], "ui": {}}]}]})
    with Tui(home, []) as t:
        t.wait_status("Row 1/50")
        t.wait("▼")
        assert cursor_id(t) == 50


def test_session_connection_tab_only(home):
    """A connection with no table or query tab open (only its connection
    tab) is restored, not lost to the Workspaces window"""
    db = make_db(home, rows=5)
    save_connection(home, db)
    with Tui(home, ["-s"]) as t:
        t.wait("testdb")
        t.keys("Enter")
        t.wait("Tables")
        assert t.quit() == 0
    conn = home.read("session.json")["workspaces"][0]["connections"][0]
    assert conn["saved_id"] == SAVED_ID and conn["tabs"] == [] and conn["order"] == 0
    with Tui(home, []) as t:
        t.wait("Tables")
        t.wait("big")
        assert "CONNECTIONS" not in t.text()
        t.keys("Enter")  # the sidebar has focus: open the table
        t.wait_status("Row 1/5")


def test_session_url_connection_kept(home):
    """A connection typed as a URL (no saved connection) survives a session
    round trip: session.json holds its password-less string"""
    db = make_db(home, rows=7)
    with Tui(home, ["-s"]) as t:  # Nothing to pick: the input has focus
        t.type("sqlite://" + db)
        t.wait("Connect to")
        t.keys("Enter")
        t.wait("Tables")
        t.keys("Enter")
        t.wait_status("Row 1/7")
        assert t.quit() == 0
    conn = home.read("session.json")["workspaces"][0]["connections"][0]
    assert "saved_id" not in conn and conn["connstr"].endswith(db)
    with Tui(home, []) as t:
        t.wait_status("Row 1/7")
        assert t.quit() == 0
    # And again: it is written back the same way
    conn = home.read("session.json")["workspaces"][0]["connections"][0]
    assert conn["connstr"].endswith(db) and conn["tabs"][0]["table"] == "big"


def test_session_bad_workspace_skipped(home):
    """A workspace that does not convert (an operator from a newer frontend)
    is skipped with a message; the others restore"""
    db = make_db(home, rows=9)
    save_connection(home, db)
    home.write("session.json", {"version": 2, "current": 1, "workspaces": [
        {"name": "Good", "connections": [{"saved_id": SAVED_ID, "current": 0,
                                          "tabs": [{"type": "table", "table": "big"}]}]},
        {"name": "Future", "connections": [{"saved_id": SAVED_ID, "current": 0, "tabs": [
            {"type": "table", "table": "big",
             "filters": [{"column": "v", "op": "someday", "value": "1"}]}]}]}]})
    with Tui(home, []) as t:
        t.wait("Row 1/9")
        t.wait("someday")
        assert "Future" in t.text()


def test_session_foreign_kept_without_connections(home):
    """Quitting with no connection keeps another frontend's members of
    session.json (the file is shared), and an empty one goes as before"""
    home.write("session.json", {"version": 2, "workspaces": [], "current": 0,
                                "gtk": {"x": 1}})
    with Tui(home, ["-s"]) as t:
        t.wait("CONNECTIONS")
        assert t.quit() == 0
    s = home.read("session.json")
    assert s["gtk"] == {"x": 1} and s["version"] == 2 and s["workspaces"] == []
    home.write("session.json", {"version": 2, "workspaces": [], "current": 0})
    with Tui(home, ["-s"]) as t:
        t.wait("CONNECTIONS")
        assert t.quit() == 0
    assert not os.path.exists(home.cfg_dir + "/session.json")
