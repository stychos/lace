"""Closing tabs and workspaces: the last tab of a connection becomes a
connection form (keep_connection_tab) or goes, a workspace without tabs
closes, and the last one follows on_last_workspace_close; a transaction is
asked about only when its connection really closes"""

import re
import time

from harness import Tui
from test_table import make_db

EXIT = re.compile(r"__LACE_EXIT__=(\d+)")


def configure(home, **general):
    cfg = home.read("config.json")
    cfg["general"].update(general)
    home.write("config.json", cfg)


def write_conns(home, db):
    home.write("connections.json", {"root": {
        "type": "folder", "name": "Connections", "expanded": True, "children": [
            {"type": "connection", "id": "11111111-0000-4000-8000-000000000001",
             "name": "Local", "driver": "sqlite", "host": "", "port": 0,
             "database": db, "user": "", "password": "", "save_password": False}]}})


def test_last_view_becomes_form(home):
    """Closing the only table of a connection leaves a form in its place
    and closes the connection"""
    db = make_db(home, rows=3)
    with Tui(home, ["-s", db]) as t:
        t.open_table("big")
        t.keys("C-w")
        t.wait("New connection")
        t.wait("URI")
        assert "Tables" not in t.text()
        assert t.quit() == 0


def test_keep_connection_tab_off(home):
    """Without keep_connection_tab the last tab goes; a workspace left with
    none closes and the other one shows"""
    configure(home, keep_connection_tab=False, auto_open_first_table=False)
    db = make_db(home, rows=3)
    write_conns(home, db)
    with Tui(home, ["-s"]) as t:
        t.wait("URI")
        t.keys("Down")
        t.wait(lambda s: re.search(r"Name\s+Local", s))
        t.keys("C-n")                        # workspace 2 with the connection
        t.wait("W2]")
        t.open_table("big")                  # the table takes the connection tab's place
        assert "t.sqlite" not in t.screen()[1]
        t.keys("C-w")                        # its last tab: workspace 2 goes
        t.wait(lambda s: "W2]" not in s and "URI" in s)
        assert "Local" in t.screen()[1]      # workspace 1's form
        assert t.quit() == 0


def test_form_closes(home):
    """A form among other tabs simply goes"""
    db = make_db(home, rows=3)
    with Tui(home, ["-s", db]) as t:
        t.wait("Tables")
        t.keys("C-t")
        t.wait("URI")
        t.keys("C-w")
        t.wait(lambda s: "URI" not in s)
        t.wait("Tables")
        assert t.quit() == 0


def test_last_tab_rules(home):
    """The only form of the only workspace: "new" gives a fresh workspace with
    a form; "quit" asks (quit_confirmation) and quits"""
    configure(home, on_last_workspace_close="new")
    with Tui(home, ["-s"]) as t:
        t.wait("URI")
        t.keys("C-w")
        t.wait("New connection")
        t.wait("URI")
        assert t.quit() == 0
    configure(home, on_last_workspace_close="quit", quit_confirmation=True)
    with Tui(home, ["-s"]) as t:
        t.wait("URI")
        t.keys("C-w")
        t.wait("Quit lace?")
        t.keys("n")
        t.wait(lambda s: "Quit lace?" not in s)
        t.wait("URI")
        t.keys("C-w")
        t.wait("Quit lace?")
        t.keys("y")
        t.wait(EXIT)
        assert EXIT.search(t.text()).group(1) == "0"
        t._expect_exit = True


def test_shared_connection_survives(home):
    """A connection used by two workspaces: closing it in one asks nothing
    and leaves the other working, transaction included; closing the last
    tab using it asks about the transaction"""
    configure(home, auto_open_first_table=False)
    db = make_db(home, rows=3)
    write_conns(home, db)
    with Tui(home, ["-s"]) as t:
        t.wait("URI")
        t.keys("Down")
        t.wait(lambda s: re.search(r"Name\s+Local", s))
        t.keys("Enter")                      # workspace 1: the connection
        t.wait("Tables")
        t.keys("C-t")
        t.wait("URI")
        t.keys("Down")
        t.wait(lambda s: re.search(r"Name\s+Local", s))
        t.keys("C-n")                        # workspace 2 shares it
        t.wait("W2]")
        t.keys("p")
        t.type("BEGIN")
        t.keys("C-r")
        t.wait_status("[TX]")
        t.keys("Escape")
        time.sleep(0.2)                      # Esc and a key at once is Alt+key
        t.keys("{")                # workspace 1, its form
        t.wait(lambda s: "W1]" in s)
        t.keys("Escape")                     # the form's panel, which leaves [ to the tab keys
        t.wait_status("folder")
        t.keys("[")                          # the connection tab
        t.wait("Tables")
        t.keys("C-w")                        # no question: workspace 2 holds it
        t.wait("URI")
        assert "transaction" not in t.text()
        t.keys("}")                          # workspace 2, its query tab
        t.wait_status("[TX]")
        t.keys("C-w")
        t.wait("unsaved content")
        t.keys("y")                          # the connection tab stays
        t.wait("Tables")
        t.keys("C-w")                        # the last tab using the connection
        t.wait("A transaction is open")
        t.keys("n")
        t.wait(lambda s: "A transaction is open" not in s)
        assert t.quit() == 0


def test_config_row(home):
    """The configuration dialog has the last-workspace setting"""
    with Tui(home, ["-s"]) as t:
        t.wait("URI")
        t.keys(",")
        t.wait("When the last workspace closes")
        assert t.quit() == 0


def test_drop_only_open_table_keeps_connection(home):
    """Dropping the table open in its connection's only tab closes that
    view, not the connection: the sidebar of tables stays"""
    db = make_db(home, rows=3)
    with Tui(home, ["-s", db]) as t:
        t.open_table("big")                  # takes the connection tab's place
        t.keys("m")
        t.wait("Table big")
        t.keys("Down", "Down", "Enter")      # Drop table…
        t.wait("Drop table big")
        t.keys("y")
        t.wait_status("Drop table: done")
        t.wait("Tables")
        assert "URI" not in t.text()
        assert t.quit() == 0
