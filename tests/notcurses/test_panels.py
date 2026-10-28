import importlib.util
import os

from nc import Nc

_spec = importlib.util.spec_from_file_location(
    "nc_test_table", os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_table.py"))
_tt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_tt)
make_db = _tt.make_db


def test_sidebar_toggle_search_open(home):
    db = make_db(home, rows=5)
    with Nc(home, ["-s", "sqlite://" + db]) as t:
        t.wait("TABLES")
        t.keys("t")                            # hide
        t.wait(lambda s: "TABLES" not in s)
        t.keys("t"); t.wait("TABLES")
        t.wait(lambda s: s.split("\n")[2].startswith(" TABLES"))   # no search field until it is called
        t.wait("/ search")                     # the focused sidebar's hints
        t.keys("/")
        t.wait(lambda s: s.split("\n")[2].startswith(" ") and not s.split("\n")[2].startswith(" TABLES"))
        t.type("bi")                           # the search field has the focus
        t.wait(lambda s: s.split("\n")[2].startswith(" bi") and "TABLES" in s.split("\n")[3])
        t.wait(lambda s: "small" not in s.split("\n")[4])          # the list is filtered
        t.keys("Enter")
        t.wait("1 of 5")
        t.wait(lambda s: s.split("\n")[2].startswith(" bi"))      # the field stays while it has text
        t.keys("Tab")                          # focus back to the sidebar
        t.keys("Escape")                       # clears the search and hides the field
        t.wait(lambda s: s.split("\n")[2].startswith(" TABLES"))
        t.keys("e")                            # a table key does nothing from the sidebar (no editor opens)
        t.keys("Escape")                       # leaves the sidebar
        t.wait("/ filter")
        t.keys("C-p"); t.type("filter tables"); t.keys("Enter")   # the palette calls the field too
        t.wait("/ search")
        t.wait(lambda s: not s.split("\n")[2].startswith(" TABLES"))
        t.keys("Escape", "Escape")             # hides it, leaves the sidebar
        t.wait("/ filter")
        t.keys("q"); assert t.quit() == 0


def test_view_switcher_and_close(home):
    db = make_db(home, rows=5)
    with Nc(home, ["-s", "sqlite://" + db]) as t:
        t.wait("big"); t.keys("Enter"); t.wait("1 of 5")
        t.keys("p"); t.wait("New query")
        t.keys("C-e")
        t.wait("Views in")
        t.wait("big")
        t.keys("Up", "Enter")                  # back to the table view
        t.wait("1 of 5")
        t.keys("C-w")                          # closes the table view, the query remains
        t.wait("New query")
        t.keys("q"); assert t.quit() == 0


def test_log_and_history(home):
    db = make_db(home, rows=5)
    with Nc(home, ["-s", "sqlite://" + db]) as t:
        t.wait("big"); t.keys("Enter"); t.wait("1 of 5")
        t.keys("F8")
        t.wait("LIMIT 1000")                   # the page SQL in the log
        t.keys("Escape")
        t.keys("p"); t.type("SELECT 42"); t.keys("C-r"); t.wait("1 of 1")
        t.keys("Escape")                       # leave the editor for the results
        t.keys("i")
        t.wait("History")
        t.wait("SELECT 42")                    # history of the connection
        t.keys("Escape")
        t.keys("q"); assert t.quit() == 0


def test_prefs_dialog_saves_to_config(home):
    with Nc(home, ["-s"]) as t:
        t.wait("palette")                      # input before the first draw is discarded
        t.keys(",")
        t.wait("Preferences")
        t.wait("Confirm deletes")
        t.keys("Space")                        # the first item, Confirm deletes, on by default: now off
        t.wait("Confirm deletes          off")
        t.keys("Escape")
        t.keys("q"); assert t.quit() == 0
    cfg = home.read("config.json")
    assert cfg["general"]["delete_confirmation"] is False
    assert "notcurses" in cfg


def row_with(s, text):
    """The screen line holding text (the first one)"""
    return [l for l in s.split("\n") if text in l][0]


def test_hotkeys_editor_adds_removes_and_resets(home):
    db = make_db(home, rows=5)
    with Nc(home, ["-s", "sqlite://" + db]) as t:
        t.wait("big"); t.keys("Enter"); t.wait("1 of 5")
        t.keys("?")
        t.wait("Hotkeys")
        t.wait("Previous tab")                 # the first action is selected (the category header is skipped)
        t.keys("Enter")                        # Enter captures a key for it
        t.wait("Press the key to add to Previous tab")
        t.keys("F6")                           # Next tab's key, the same category: refused
        t.wait("F6 is already bound to Next tab")
        t.keys("Enter"); t.wait("Press the key"); t.keys("F12")
        t.wait(lambda s: "F12" in row_with(s, "Previous tab"))
        t.keys("j")                            # the lists take the vim keys too
        t.keys("Enter"); t.wait("Press the key to add to Next tab")
        t.keys("Escape")                       # cancels the capture only
        t.wait(lambda s: "Press the key" not in s and "Next tab" in s)
        t.keys("Escape")                       # closes the editor
        t.wait("/ filter")
        t.keys("F12")                          # the new key works at once: Previous tab, with one view
        t.wait_status("Only one tab")
        t.keys("?"); t.wait("Hotkeys")
        t.keys("-")                            # removes the last key
        t.wait(lambda s: "F12" not in row_with(s, "Previous tab"))
        t.keys("Enter"); t.wait("Press the key"); t.keys("F12")
        t.wait(lambda s: "F12" in row_with(s, "Previous tab"))
        t.keys("r")                            # the action's defaults
        t.wait("Previous tab: default keys")
        t.wait(lambda s: "F12" not in row_with(s, "Previous tab"))
        t.keys("Enter"); t.wait("Press the key"); t.keys("F12")
        t.wait(lambda s: "F12" in row_with(s, "Previous tab"))
        t.keys("R")                            # every action's defaults, after a confirmation
        t.wait("Give every action its default keys?")
        t.keys("Escape")
        t.wait(lambda s: "F12" in row_with(s, "Previous tab"))
        t.keys("Escape")
        t.keys(","); t.wait("Preferences")     # the editor opens from the preferences too
        t.keys("End", "Enter")
        t.wait("Press the key" if False else "Enter add key")
        t.keys("Escape", "Escape")
        t.keys("q"); assert t.quit() == 0
    cfg = home.read("config.json")
    assert cfg["hotkeys"]["prev_tab"] == ["[", "F7", "F12"]


def test_spatial_navigation(home):
    """The arrows cross panel edges as in the ncurses TUI: Left from the
    grid's first column (or the filter panel's first field, or a query's
    results, or the editor's start) into the sidebar, Right back to where
    the focus came from; Up from the grid's top into the filter panel and
    Down from its last row back; Up from the results into the editor and
    Down from its last line (or Right at its end) back into the results"""
    db = make_db(home, rows=5)
    grid, sidebar, panel = "/ filter", "/ search", "Esc table"
    with Nc(home, ["-s", "sqlite://" + db]) as t:
        t.wait("big"); t.keys("Enter"); t.wait("1 of 5")
        t.wait_status(grid)
        t.keys("Right"); t.keys("Left")              # back to the first column
        t.keys("Left"); t.wait_status(sidebar)       # over the left edge
        t.keys("Right"); t.wait_status(grid)
        t.keys("/"); t.wait_status(panel)            # the filter panel, focused
        t.keys("Down"); t.wait_status(grid)          # below its last row
        t.keys("Up"); t.wait_status(panel)           # over the grid's top edge
        for _ in range(4):
            t.keys("Left")                           # its fields, then the sidebar
        t.wait_status(sidebar)
        t.keys("Right"); t.wait_status(panel)        # back to the panel it came from
        t.keys("Escape"); t.wait_status(grid)
        t.keys("p"); t.wait("⌃R run")
        t.type("SELECT 1")
        t.keys("C-r"); t.wait("1 row")
        editor, results = "⌃R run", "Enter editor"
        t.wait_status(editor)
        t.keys("Down"); t.wait_status(results)       # below the editor's last line
        t.keys("Left"); t.wait_status(sidebar)       # the results' left edge
        t.keys("Right"); t.wait_status(results)
        t.keys("Up"); t.wait_status(editor)          # over the results' top edge
        t.keys("End"); t.keys("Right"); t.wait_status(results)   # past the text's end
        t.keys("Up"); t.keys("Home"); t.keys("Left"); t.wait_status(sidebar)   # before its start
        t.keys("q"); assert t.quit() == 0
