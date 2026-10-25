"""The tabs strip and the filter panel (preferences tabs_strip and filters_strip)"""
import importlib.util
import os
import re

from harness import sqlite_db
from nc import Nc

_spec = importlib.util.spec_from_file_location(
    "nc_test_table", os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_table.py"))
_tt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_tt)
at, make_db, cursor_id, wait_id, open_big, sidebar_cols = (
    _tt.at, _tt.make_db, _tt.cursor_id, _tt.wait_id, _tt.open_big, _tt.sidebar_cols)


def strip(t):
    """The tabs strip's line (of an Nc, or of a screen text)"""
    return (t if isinstance(t, str) else t.text()).split("\n")[1]


def italic_labels(t):
    """The strip's labels drawn in italics (the preview tab), from a capture with escapes"""
    raw = t.tmux("capture-pane", "-t", "t", "-p", "-e", capture=True).split("\n")[1]
    labels = [m.group(1).strip() for m in re.finditer(r"\x1b\[(?:1;)?3m(?:\x1b\[[0-9;]*m)*([^\x1b]*)", raw)]
    return [l for l in labels if l]


def test_tabs_strip_lists_views_and_clicks(home):
    with open_big(home, 10) as t:
        t.wait(lambda s: " big ×" in s.split("\n")[1])
        assert "(1 of 1)" not in t.text().split("\n")[0]   # the strip shows the count instead
        t.keys("p"); t.wait("New query")
        t.wait(lambda s: "big │ New query ×" in s.split("\n")[1])
        t.click(strip(t).index("big") + 1, 1)             # back to the table by a click
        t.wait(lambda s: "big × │ New query" in s.split("\n")[1])
        t.wait(at(1, 10))
        t.keys("]")                                       # the keys still switch
        t.wait(lambda s: "big │ New query ×" in s.split("\n")[1])
        t.click(strip(t).index("×"), 1)                   # the current tab's close mark
        t.wait(lambda s: " big ×" in s.split("\n")[1] and "New query" not in s.split("\n")[1])
        t.keys("p"); t.wait("New query")
        t.click(strip(t).index("big") + 1, 1, button=1)   # a middle click closes another tab
        t.wait(lambda s: "big" not in s.split("\n")[1] and "New query ×" in s.split("\n")[1])
        t.keys("Escape", "q"); assert t.quit() == 0


def test_tabs_strip_spans_connections(home):
    db = make_db(home, rows=10)
    with Nc(home, ["-s", "sqlite://" + db, "sqlite://" + db]) as t:   # the last tab opened is current
        t.wait("big"); t.keys("Enter"); t.wait(at(1, 10))
        t.wait(lambda s: strip(s).startswith(" t.sqlite │ big ×"))  # one flat row; a tab without views is a cell of its own
        t.keys("]")                                                # ...which the tab keys reach
        t.wait(lambda s: s.split("\n")[0].startswith(" Workspace ▸ t.sqlite") and "t.sqlite 2" not in s.split("\n")[0])
        t.wait(lambda s: strip(s).startswith(" t.sqlite × │ big"))
        t.keys("M-2")                                              # Alt+N: the strip's Nth cell
        t.wait(lambda s: strip(s).startswith(" t.sqlite │ big ×"))
        t.keys("M-1")
        t.wait(lambda s: strip(s).startswith(" t.sqlite × │ big"))
        t.wait("big"); t.keys("Enter"); t.wait(at(1, 10))
        t.wait(lambda s: strip(s).startswith(" big × │ big"))
        t.keys("]")                                                # the bracket keys cross the tabs
        t.wait(lambda s: strip(s).startswith(" big │ big ×"))
        t.wait(lambda s: "t.sqlite 2 ▸ big" in s.split("\n")[0])
        t.keys("]")                                                # ...and wrap
        t.wait(lambda s: strip(s).startswith(" big × │ big"))
        t.keys("C-PageDown")                                       # the Ctrl keys walk the same row
        t.wait(lambda s: strip(s).startswith(" big │ big ×"))
        t.keys("C-PageUp")
        t.wait(lambda s: strip(s).startswith(" big × │ big"))
        t.click(strip(t).index("│") + 2, 1)                       # a click on the other tab's view
        t.wait(lambda s: strip(s).startswith(" big │ big ×"))
        t.keys("C-e")                                              # the switcher lists every tab's views
        t.wait("Views")
        t.wait(lambda s: s.count(" big") >= 2)
        t.keys("Up", "Enter")                                      # the first tab's view
        t.wait(lambda s: strip(s).startswith(" big × │ big"))
        t.click(strip(t).index("│") + 2, 1, button=1)             # a middle click closes the other tab's
        t.wait(lambda s: strip(s).startswith(" big │ New connection") and "t.sqlite 2" not in s.split("\n")[0])   # only view: the tab turns into a form
        t.keys("q"); assert t.quit() == 0


def test_new_tab_and_close_tab_keys(home):
    db = make_four_tables(home)
    with Nc(home, ["-s", "sqlite://" + db]) as t:
        t.wait("last"); t.keys("Enter")                   # big is open and current
        t.wait(lambda s: strip(s).startswith(" big ×"))
        t.keys("Tab")                                     # the sidebar takes the keys
        t.wait("+ new tab")
        t.keys("j", "+")                                  # the next table opens behind the current view
        t.wait_status("Opened last in a new tab")
        t.wait(lambda s: strip(s).startswith(" big × │ last"))
        t.keys("+")                                       # again: only reported
        t.wait_status("last is already open")
        t.keys("Escape")                                  # back to the grid
        t.keys(":"); t.wait("Command palette"); t.keys("Escape")   # the vim-style palette key
        t.keys("-")                                       # close tab: the current view goes
        t.wait(lambda s: strip(s).startswith(" last ×"))
        t.keys("C-w")                                     # the chord does the same; the last view closes the connection tab
        t.wait("RECENT")
        t.keys("q"); assert t.quit() == 0


def test_sidebar_takes_the_navigation_keys(home):
    db = make_four_tables(home)
    with Nc(home, ["-s", "sqlite://" + db]) as t:
        t.wait("last")
        t.keys("j", "j", "Enter")                        # no view: the list (big, last, small, tiny) takes j/k
        t.wait(lambda s: "\u25b8 small" in s.split("\n")[0])
        t.keys("Tab")                                     # the focused sidebar takes them too
        t.keys("k", "Enter")
        t.wait(lambda s: "\u25b8 last" in s.split("\n")[0])
        t.keys("Tab", "G", "Enter")                       # G: the last entry
        t.wait(lambda s: "\u25b8 tiny" in s.split("\n")[0])
        t.keys("Tab", "g", "Enter")                       # g: the first
        t.wait(lambda s: "\u25b8 big" in s.split("\n")[0])
        t.keys("q"); assert t.quit() == 0


def make_four_tables(home):
    return sqlite_db(home.file("t.sqlite"), """
        CREATE TABLE big (id INTEGER PRIMARY KEY, v INTEGER, name TEXT);
        INSERT INTO big VALUES (1, 1, 'name1'), (2, 2, 'name2');
        CREATE TABLE small (id INTEGER PRIMARY KEY, w TEXT);
        INSERT INTO small VALUES (1, 'x');
        CREATE TABLE tiny (id INTEGER PRIMARY KEY);
        CREATE TABLE last (id INTEGER PRIMARY KEY);
    """)


def side(t, name):
    """(x, y) of a table's entry in the sidebar (the header and the strip name tables too)"""
    for y, line in enumerate(t.screen()):
        if y >= 3 and name in line[:sidebar_cols(t)]:
            return line.index(name), y
    raise AssertionError(f"{name} not in the sidebar\n{t.text()}")


def test_sidebar_single_click_previews_double_click_keeps(home):
    db = make_four_tables(home)
    with Nc(home, ["-s", "sqlite://" + db]) as t:
        t.wait("last"); t.keys("Enter")                   # Enter opens for keeps
        t.wait(at(1, 2))
        t.click(*side(t, "small"))                        # a single click: the preview tab
        t.wait(lambda s: "big │ small ×" in strip(s))
        t.wait(at(1, 1))
        assert italic_labels(t) == ["small"]              # the preview tab's label is in italics
        t.click(*side(t, "tiny"))                         # the next single click replaces it
        t.wait(lambda s: "big │ tiny ×" in strip(s) and "small" not in strip(s))
        t.click(*side(t, "small"), double=True)           # a double click opens for keeps
        t.wait(lambda s: "big │ small ×" in strip(s) and "tiny" not in strip(s))
        assert italic_labels(t) == []                     # kept: upright
        t.click(*side(t, "tiny"))                         # so this one gets a new preview tab
        t.wait(lambda s: "big │ small │ tiny ×" in strip(s))
        t.keys("o")                                       # sorting the preview keeps it
        t.wait("\u2191id")
        t.click(*side(t, "last"))
        t.wait(lambda s: "big │ small │ tiny │ last ×" in strip(s))
        t.click(*side(t, "big"))                          # an open table is switched to, as it is
        t.wait(lambda s: "big × │ small │ tiny │ last" in strip(s))
        t.keys("q"); assert t.quit() == 0


def test_tabs_strip_scrolls_to_the_current_view(home):
    db = make_db(home, rows=10)
    with Nc(home, ["-s", "sqlite://" + db], cols=48) as t:
        t.wait("big"); t.keys("Enter"); t.wait(at(1, 10))
        t.wait(lambda s: " big ×" in s.split("\n")[1])
        for _ in range(4):
            t.keys("p"); t.wait("New query"); t.keys("Escape")   # Esc: the editor gives the keys back
        t.wait(lambda s: s.split("\n")[1].startswith("‹") and "New query ×" in s.split("\n")[1])
        t.keys("[", "[", "[", "[")                        # back to the first
        t.wait(lambda s: s.split("\n")[1].startswith(" big ×") and s.split("\n")[1].rstrip().endswith("›"))
        t.keys("q"); assert t.quit() == 0


def test_filter_panel_edit_add_remove_clear(home):
    with open_big(home, 100) as t:
        t.keys("Right", "/")                              # a row for v, its value field focused
        t.wait("Filters")
        t.wait(lambda s: "v " in s.split("\n")[3] and "=" in s.split("\n")[3])
        t.type("42"); t.keys("Enter")                     # typing edits the value; Enter applies
        t.wait("Filters (1)")
        t.wait("v = 42")
        assert wait_id(t, 42) == 42
        t.wait(at(1, 1))
        t.keys("+")                                       # a second row, ready to type
        t.wait(lambda s: len([l for l in s.split("\n")[3:6] if " = " in l or "=" in l]) >= 2)
        t.type("41"); t.keys("Enter")                     # v = 42 AND v = 41: nothing
        t.wait("Filters (2)")
        t.wait(at(0, 0))                                  # "0 of 0"
        t.keys("-")                                       # remove the cursor row: back to one match
        t.wait("Filters (1)")
        t.wait(at(1, 1))
        t.keys("-")                                       # remove the last one: the panel hides itself
        t.wait(lambda s: "Filters" not in s)
        t.wait("/ filter")                                # and the grid has the focus
        t.wait(at(1, 100))
        t.keys("/")                                       # a blank row for the cursor column again
        t.wait("Filters")
        t.keys("+")                                       # a blank row does not stop another being added
        t.wait(lambda s: len([l for l in s.split("\n")[3:6] if "=" in l]) >= 2)
        t.type("42"); t.keys("Enter")
        t.wait("Filters (1)")                             # one active, one blank
        t.wait(at(1, 1))
        t.keys("Escape")                                  # the panel stays, the grid has the focus
        t.wait("/ filter")
        t.keys("X")                                       # clear from the grid
        t.wait(lambda s: "v = 42" not in s)
        t.wait(at(1, 100))
        t.keys("/")                                       # focus the panel again
        t.wait("Enter edit")
        t.keys("/")                                       # and hide it
        t.wait(lambda s: "Filters" not in s)
        t.keys("q"); assert t.quit() == 0


def test_filter_panel_pickers_and_tab_ring(home):
    with open_big(home, 100) as t:
        t.keys("Right", "Right", "/")                     # name
        t.wait("Filters")
        t.keys("BTab")                                   # the operator field
        t.keys("Enter")                                   # its picker
        t.wait("Operator")
        t.type("contains"); t.keys("Enter")
        t.wait(lambda s: "contains" in s.split("\n")[3])
        t.keys("Tab")                                     # the value
        t.type("9"); t.keys("Enter")
        t.wait("name contains '9'")
        t.wait(at(1, 19))
        t.keys("BTab", "BTab")                          # the column field
        t.keys("Enter")
        t.wait("Column")
        t.wait("Raw SQL")
        t.keys("Escape")
        t.wait(lambda s: "Raw SQL" not in s)
        t.keys("M-Right", "M-Right")                      # Alt+Right cycles the operator from any field
        t.wait(lambda s: "ends with" in s.split("\n")[3])
        t.wait("name ends with '9'")
        t.wait(at(1, 10))
        t.keys("BTab")                                   # before the first field: the sidebar
        t.keys("/"); t.type("zz")                         # / calls its search field, which takes the typing
        t.wait(lambda s: s.split("\n")[2].startswith(" zz"))
        t.keys("BSpace", "BSpace")
        t.keys("Tab")                                     # sidebar -> panel
        t.wait("Enter edit")
        t.keys("Tab", "Tab", "Tab")                       # past the last field: the grid
        t.wait("/ filter")
        t.keys("BTab")                                   # grid -> panel (its last field)
        t.wait("Enter edit")
        t.keys("q")                                       # a letter on the value field types, no quit
        t.wait(lambda s: "q" in s.split("\n")[3])
        t.keys("Escape")                                  # cancels the edit
        t.keys("Escape")                                  # leaves the panel
        t.keys("q"); assert t.quit() == 0


def test_filter_panel_raw_and_between(home):
    with open_big(home, 100) as t:
        t.keys("/")
        t.wait("Filters")
        t.keys("BTab", "BTab")                          # the column field
        t.keys("Enter"); t.wait("Raw SQL")
        t.type("raw"); t.keys("Enter")                    # the last row: Raw SQL
        t.wait("(raw SQL)")
        t.keys("Tab")                                     # the condition (no operator field)
        t.type("id < 4"); t.keys("Enter")
        t.wait("(id < 4)")
        t.wait(at(1, 3))
        t.keys("+")                                       # a second row for the cursor column id
        t.keys("Escape")                                  # stop typing
        t.keys("Left")                                    # the operator field
        for _ in range(11):                               # = ... regex, between
            t.keys("Space")
        t.wait(lambda s: "between" in s)
        t.keys("Tab"); t.type("2"); t.keys("Tab"); t.type("3"); t.keys("Enter")
        t.wait("id between 2 and 3")
        t.wait(at(1, 2))
        t.keys("Escape", "q"); assert t.quit() == 0


def test_strips_off_by_preference(home):
    home.write("config.json", {"notcurses": {"tabs_strip": False, "filters_strip": False}})
    with open_big(home, 10) as t:
        t.wait("big (1 of 1)")                            # no strip: the header counts the views
        assert " big ×" not in t.text()
        t.keys("/")
        t.wait("Operator:")                               # the overlay editor instead of the panel
        t.keys("Escape")
        t.keys(",")
        t.wait("Tabs strip               off")
        t.wait("Filters strip            off")
        t.keys("Escape")
        t.keys("C-p"); t.type("tabs strip"); t.keys("Enter")   # the palette toggles it
        t.wait(lambda s: " big ×" in s.split("\n")[1])
        t.keys("q"); assert t.quit() == 0
    cfg = home.read("config.json")
    assert cfg["notcurses"]["tabs_strip"] is True and cfg["notcurses"]["filters_strip"] is False


def test_filter_panel_restored_with_the_session(home):
    db = make_db(home, rows=50)
    home.write("config.json", {"general": {"restore_session": True}})
    with Nc(home, ["sqlite://" + db]) as t:
        t.wait("big"); t.keys("Enter"); t.wait(at(1, 50))
        t.keys("Right", "/"); t.wait("Filters")
        t.type("7"); t.keys("Enter")
        t.wait(at(1, 1))
        t.keys("+")                                       # a blank row: not saved
        t.keys("Escape")
        assert t.quit() == 0
    with Nc(home, []) as t:
        t.wait("Filters (1)")
        t.wait("v = 7")
        t.wait(at(1, 1))
        assert len([l for l in t.text().split("\n")[3:6] if "v " in l and "=" in l]) == 1
        t.keys("q"); assert t.quit() == 0
