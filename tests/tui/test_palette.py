"""The command palette (Ctrl+P, :), as the notcurses TUI's: one filtered
list of the actions that mean something with what is open, the tables,
the connection's tabs, workspaces, connections and the on/off
preferences. An action runs through its first key."""

from harness import Tui
from test_table import cursor_id, make_db


def test_lists_by_what_is_open(home):
    """A bare connection lists no grid moves; an open table does"""
    db = make_db(home, rows=3)
    with Tui(home, ["-s", "sqlite://" + db], cols=110, rows=34) as t:
        t.wait("TABLES")
        t.keys("C-p")
        t.wait("Command palette")
        t.type("move down")
        t.wait("> move down")
        assert "Move down" not in t.text()
        t.keys("Escape")
        t.wait(lambda s: "Command palette" not in s)
        t.open_table("big")
        t.wait_header("1 of 3")
        t.keys("C-p")
        t.wait("Command palette")
        t.type("move down")
        t.wait("Move down")
        t.keys("Enter")                     # runs j: the grid moves
        t.wait(lambda s: "Command palette" not in s)
        t.wait_header("2 of 3")
        assert t.quit() == 0


def test_colon_and_tables(home):
    """: opens it from the grid; an "Open table" row opens that table"""
    db = make_db(home, rows=3)
    with Tui(home, ["-s", "sqlite://" + db], cols=110, rows=34) as t:
        t.wait("TABLES")
        t.keys(":")
        t.wait("Command palette")
        t.type("open table big")
        t.wait("Open table big")
        t.keys("Enter")
        t.wait_header("1 of 3")
        assert t.quit() == 0


def test_sort_picker(home):
    """Sort by column: Enter cycles a column up, down, off; Esc applies"""
    db = make_db(home, rows=3)
    with Tui(home, ["-s", "sqlite://" + db], cols=110, rows=34) as t:
        t.open_table("big")
        t.wait_header("1 of 3")
        assert cursor_id(t) == 1
        t.keys("C-p")
        t.wait("Command palette")
        t.type("sort by")
        t.wait("Sort by column")
        t.keys("Enter")
        t.wait("Enter cycle")
        t.keys("Down", "Enter")             # v ascending
        t.wait(lambda s: "v" in s and "↑" in s)
        t.keys("Enter")                     # v descending
        t.wait("↓")
        t.keys("Escape")                    # applies
        t.wait(lambda s: "Enter cycle" not in s)
        t.wait(lambda s: cursor_id(t) == 3)
        assert t.quit() == 0


def test_runs_inside_the_editor(home):
    """Ctrl+P works while typing SQL; Execute query runs the statement,
    and a plain : in the editor is typed"""
    db = make_db(home, rows=3)
    with Tui(home, ["-s", "sqlite://" + db], cols=110, rows=34) as t:
        t.wait("TABLES")
        t.keys("p")
        t.wait("New query")
        t.type("SELECT 40 + 2 AS x -- :")
        t.wait("-- :")
        assert "Command palette" not in t.text()
        t.keys("C-p")
        t.wait("Command palette")
        t.type("execute query")
        t.wait("Execute query")
        t.keys("Enter")
        t.wait(lambda s: "Command palette" not in s and "42" in s)
        assert t.quit() == 0


def test_preference_toggle(home):
    """A preference row flips it, shows it and saves config.json"""
    db = make_db(home, rows=3)
    with Tui(home, ["-s", "sqlite://" + db], cols=110, rows=34) as t:
        t.open_table("big")
        t.wait_header("1 of 3")
        assert t.workspace() == "Workspace"   # the header line
        t.keys("C-p")
        t.wait("Command palette")
        t.type("show header")
        t.wait("Show header: on")
        t.keys("Enter")
        t.wait(lambda s: "Workspace \u25b8" not in s)
        t.wait_status("Show header: off")
        config = home.read("config.json")
        assert config["general"]["show_header"] is False
        assert t.quit() == 0


def test_workspaces_and_close(home):
    """New workspace from the palette; a click outside closes it"""
    db = make_db(home, rows=3)
    with Tui(home, ["-s", "sqlite://" + db], cols=110, rows=34) as t:
        t.wait("TABLES")
        t.keys("C-p")
        t.wait("Command palette")
        t.click(1, 30)                      # outside the frame
        t.wait(lambda s: "Command palette" not in s)
        t.keys("C-p")
        t.wait("Command palette")
        t.type("new workspace")
        t.wait("New workspace")
        t.keys("Enter")
        t.wait_workspace("Workspace 2")
        t.wait("URI")                       # its connection form
        t.keys("C-p")                       # from a form too
        t.wait("Command palette")
        t.type("switch to")
        t.wait("Switch to")
        t.keys("Enter")                     # the first: workspace 1
        t.wait_workspace("Workspace")
        assert t.quit() == 0
