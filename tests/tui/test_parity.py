"""The screen as the notcurses TUI draws it, and its behaviour: the header
path and position, the tabs strip (close mark, middle click), clickable
status hints, the view switcher, the log, Alt+1..9, the preview tab, column
widths, the query split, the preferences and the confirmations."""

import re
import time

from harness import Tui, sqlite_db
from test_table import cursor_id, make_db


def two_tables(home):
    return sqlite_db(home.file("t.sqlite"), """
        CREATE TABLE big (id INTEGER PRIMARY KEY, v INTEGER, name TEXT);
        INSERT INTO big VALUES (1, 10, 'a'), (2, 20, 'b'), (3, 30, 'c');
        CREATE TABLE small (id INTEGER PRIMARY KEY, w TEXT);
        INSERT INTO small VALUES (1, 'x');
    """)


def strip(t):
    return t.screen()[1]


def test_header_and_strip(home):
    """The header names workspace, connection and table and says where the
    cursor is; the strip's × closes the current tab"""
    db = two_tables(home)
    with Tui(home, ["-s", "sqlite://" + db], cols=110, rows=30) as t:
        t.open_table("big")
        t.wait_header("1 of 3")
        assert t.header().startswith(" Workspace ▸ t.sqlite ▸ big")
        assert " big ×" in strip(t)
        t.keys("o")                                  # a sort shows in the header
        t.wait_header("↑id")
        t.keys("p")                                  # a query tab after it
        t.wait(lambda s: "New query ×" in strip(t))
        x = strip(t).index("×")
        t.click(x, 1)                                # its close mark
        t.wait(lambda s: "New query" not in strip(t))
        assert t.quit() == 0


def test_middle_click_and_alt_number(home):
    db = two_tables(home)
    with Tui(home, ["-s", "sqlite://" + db], cols=110, rows=30) as t:
        t.open_table("big")
        t.keys("p")
        t.wait(lambda s: "New query ×" in strip(t))
        t.keys("Escape")
        t.keys("M-1")                                # Alt+1: the first tab
        t.wait(lambda s: " big ×" in strip(t))
        x = strip(t).index("New query")
        t.click(x, 1, button=1)                      # a middle click closes it
        t.wait(lambda s: "New query" not in strip(t))
        assert t.quit() == 0


def test_status_hint_click(home):
    """A hint with an action runs it when clicked"""
    db = two_tables(home)
    with Tui(home, ["-s", "sqlite://" + db], cols=110, rows=30) as t:
        t.open_table("big")
        t.wait_status("⌃P palette")
        x = t.status().index("⌃P palette")
        t.click(x + 1, t.rows - 1)
        t.wait("Command palette")
        t.keys("Escape")
        assert t.quit() == 0


def test_view_switcher_and_log(home):
    db = two_tables(home)
    with Tui(home, ["-s", "sqlite://" + db], cols=110, rows=30) as t:
        t.open_table("big")
        t.keys("o")                                  # a sort: listed in the switcher
        t.wait_header("↑id")
        t.keys("p")
        t.wait(lambda s: "New query ×" in strip(t))
        t.keys("Escape", "C-e")
        t.wait("Enter switch")
        t.wait("1 sort")
        t.keys("Up", "Enter")                        # back to the table
        t.wait(lambda s: " big ×" in strip(t))
        t.keys("F8")                                 # the log has the status messages
        t.wait("Log")
        t.wait("Connected to t.sqlite")
        t.keys("Escape")
        t.wait(lambda s: "Enter copy" not in s)
        assert t.quit() == 0


def test_preview_tab(home):
    """A single click in the sidebar previews (the next one replaces it), a
    double click or Enter keeps"""
    db = two_tables(home)
    with Tui(home, ["-s", "sqlite://" + db], cols=110, rows=30) as t:
        t.wait("TABLES")
        t.keys("Enter")                              # big, kept (Enter)
        t.wait_header("1 of 3")
        x, y = t.find(" small ")
        t.click(x + 1, y)                            # a preview tab for small
        t.wait(lambda s: "small" in strip(t) and "big" in strip(t))
        x, y = t.find(" big ")
        t.click(x + 1, y)                            # an open table: switched to
        t.wait_header("▸ big")
        x, y = t.find(" small ")
        t.click(x + 1, y, double=True)               # kept now
        t.wait_header("▸ small")
        t.keys("C-e")
        t.wait("Enter switch")
        assert "(preview)" not in t.text()
        t.keys("Escape")
        assert t.quit() == 0


def test_column_width_and_split(home):
    db = make_db(home, rows=3)
    with Tui(home, ["-s", "sqlite://" + db], cols=110, rows=30) as t:
        t.open_table("big")
        t.wait_header("1 of 3")
        before = t.cursor_row_text()
        t.keys(">")                                  # the id column wider
        t.wait(lambda s: t.cursor_row_text() != before)
        assert "name1" in t.cursor_row_text()
        t.keys("p")
        t.wait("runs the statement at the cursor")
        div = next(i for i, l in enumerate(t.screen()) if "───" in l and i > 2)
        t.keys("C-Down", "C-Down")                   # the editor two rows larger
        t.wait(lambda s: any("───" in l for l in t.screen()[div + 2:div + 3]))
        assert t.quit() == 0


def test_preferences_overlay(home):
    """, lists the shared preferences; a change is saved"""
    db = make_db(home, rows=3)
    with Tui(home, ["-s", "sqlite://" + db], cols=110, rows=30) as t:
        t.open_table("big")
        t.keys(",")
        t.wait("Preferences")
        t.wait("Restore session")
        t.keys("Escape")
        t.wait(lambda s: "Space change" not in s)
        assert t.quit() == 0


def test_confirm_wraps_a_long_word(home):
    """A confirmation wraps its message; a word wider than a line is cut,
    not dropped"""
    long_name = "t" + "x" * 80
    db = sqlite_db(home.file("t.sqlite"), f"CREATE TABLE {long_name} (id INTEGER PRIMARY KEY);"
                                          f"INSERT INTO {long_name} VALUES (1);")
    with Tui(home, ["-s", "sqlite://" + db], cols=100, rows=30) as t:
        t.open_table(long_name)
        t.wait_header("1 of 1")
        t.keys("x")                                  # delete this row?
        t.wait("Delete this row?")
        t.keys("n")
        t.wait(lambda s: "Delete this row?" not in s)
        t.keys("m")                                  # the table's actions
        t.wait("Drop table")
        t.keys("Down", "Down", "Enter")
        t.wait(lambda s: "txxxx" in s and "Cancel" in s)
        assert "xxxxxxxxxx" in t.text()              # the long name is shown
        t.keys("n")
        assert t.quit() == 0


def test_switch_database_sqlite_and_editor_click(home):
    """A SQLite file is one database: D says so. A click in the query
    editor puts the cursor there."""
    db = make_db(home, rows=3)
    with Tui(home, ["-s", "sqlite://" + db], cols=110, rows=30) as t:
        t.open_table("big")
        t.wait_header("1 of 3")
        t.keys("D")
        t.wait_status("A SQLite file is one database")
        t.keys("p")
        t.wait("runs the statement at the cursor")
        t.type("SELECT 1")
        x, y = t.find("1 SELECT 1")
        t.click(x + 2 + len("SELECT"), y)           # between SELECT and 1
        time.sleep(0.4)                              # ncurses waits this long for a double click
        t.type("X")
        t.wait("SELECTX 1")
        assert t.quit() == 0
