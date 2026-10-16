"""Editing: inline edit, the cell editor, NULL/empty, delete, add row, copy and paste"""
import importlib.util
import os
import re

from harness import sqlite_db, sqlite_exec, sqlite_rows
from nc import Nc

# test_table.py of this directory, by path: tests/tui has a module of the same name
_spec = importlib.util.spec_from_file_location(
    "nc_test_table", os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_table.py"))
_tt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_tt)
at, make_db, make_slow_db = _tt.at, _tt.make_db, _tt.make_slow_db


def open_big(home, rows):
    db = make_db(home, rows=rows)
    t = Nc(home, ["-s", "sqlite://" + db])
    t.wait("big")
    t.keys("Enter")
    t.wait(at(1, rows))
    return t, db


def test_inline_edit_writes_verified(home):
    t, db = open_big(home, 5)
    with t:
        t.keys("Right", "Right", "Enter")      # name column, inline edit
        t.keys("End")
        for _ in range(8):                     # "name1" is 5 characters; a few extra are harmless
            t.keys("BSpace")
        t.type("edited")
        t.keys("Enter")
        t.wait_status("Updated")
        assert sqlite_rows(db, "SELECT name FROM big WHERE id = 1") == [("edited",)]
        t.wait("edited")
        # Unchanged text writes nothing: no "Updated" message this time
        t.keys("Enter", "Enter")
        t.wait_status("No change")
        t.keys("q"); assert t.quit() == 0


def test_inline_edit_escape_cancels(home):
    t, db = open_big(home, 3)
    with t:
        t.keys("Right", "Right", "Enter")
        t.type("zzz")
        t.wait("name1zzz")
        t.keys("Escape")
        t.wait(lambda s: "name1zzz" not in s)
        assert sqlite_rows(db, "SELECT name FROM big WHERE id = 1") == [("name1",)]
        t.keys("q"); assert t.quit() == 0


def test_inline_null_and_empty(home):
    t, db = open_big(home, 3)
    with t:
        t.keys("Right", "Right", "Enter", "C-n")     # NULL from inside the edit
        t.wait_status("Updated")
        assert sqlite_rows(db, "SELECT name IS NULL FROM big WHERE id = 1") == [(1,)]
        t.keys("Enter", "C-d")                       # empty from inside the edit
        t.wait_status("Updated")
        assert sqlite_rows(db, "SELECT name, typeof(name) FROM big WHERE id = 1") == [("", "text")]
        t.keys("q"); assert t.quit() == 0


def test_modal_editor_null_and_empty(home):
    t, db = open_big(home, 5)
    with t:
        t.keys("Right", "Right", "e")          # the floating editor
        t.wait("Editor")
        t.keys("C-n")                          # NULL
        t.wait_status("Updated")
        assert sqlite_rows(db, "SELECT name IS NULL FROM big WHERE id = 1") == [(1,)]
        t.wait("NULL")
        t.keys("d")                            # set empty, inline
        t.wait_status("Updated")
        assert sqlite_rows(db, "SELECT name FROM big WHERE id = 1") == [("",)]
        t.keys("q"); assert t.quit() == 0


def test_modal_editor_multiline(home):
    t, db = open_big(home, 3)
    with t:
        t.keys("Right", "Right", "e")
        t.wait("Editor")
        t.keys("End", "M-Enter")               # a newline (Ctrl+J is Enter on a legacy terminal)
        t.type("two")
        t.keys("Up", "Home", "Delete")         # first line loses its "n"
        t.keys("Enter")                        # save
        t.wait_status("Updated")
        assert sqlite_rows(db, "SELECT name FROM big WHERE id = 1") == [("ame1\ntwo",)]
        t.keys("q"); assert t.quit() == 0


def test_stale_row_is_refused(home):
    t, db = open_big(home, 5)
    with t:
        sqlite_exec(db, "UPDATE big SET name = 'changed behind' WHERE id = 1")
        t.keys("Right", "Right", "Enter")
        t.type("x")
        t.keys("Enter")
        t.wait_status("changed")               # the verify step saw another value
        assert sqlite_rows(db, "SELECT name FROM big WHERE id = 1") == [("changed behind",)]
        t.keys("q"); assert t.quit() == 0


def test_text_in_a_number_column_is_written_as_text(home):
    t, db = open_big(home, 3)
    with t:
        t.keys("Right", "Enter", "End", "x", "Enter")   # v is an INTEGER column; SQLite keeps the text
        t.wait_status("Updated")
        assert sqlite_rows(db, "SELECT v, typeof(v) FROM big WHERE id = 1") == [("1x", "text")]
        t.keys("q"); assert t.quit() == 0


def test_empty_inline_edit_of_null_is_no_change(home):
    t, db = open_big(home, 3)
    with t:
        t.keys("Right", "Right", "C-n")                  # NULL (set-null action)
        t.wait_status("Updated")
        t.keys("Enter", "x", "BSpace", "Enter")          # typed and cleared: still nothing
        t.wait_status("No change")
        assert sqlite_rows(db, "SELECT name IS NULL FROM big WHERE id = 1") == [(1,)]
        t.keys("q"); assert t.quit() == 0


def test_edit_refused_while_reloading(home):
    db = make_slow_db(home, 200000)
    t = Nc(home, ["-s", "sqlite://" + db])
    with t:
        t.wait("big"); t.keys("Enter"); t.wait(at(1, 200000))
        t.keys("Right", "o")                             # sort by v, the slow generated column
        t.keys("Enter")
        t.wait_status("Loading")                         # refused: the reload is still in flight
        t.keys("Escape")
        t.wait_status(re.compile(r"[Cc]ancel"), timeout=15)   # and it was: Esc cancelled it
        t.keys("q"); assert t.quit() == 0


def test_edit_pinned_to_its_row(home):
    t, db = open_big(home, 3)
    with t:
        t.keys("Right", "Right", "Down", "Enter")        # row 2
        t.type("Z")
        sqlite_exec(db, "INSERT INTO big VALUES (0, 0, 'zero')")   # behind the TUI's back
        t.keys("Enter")
        t.wait_status("Updated")
        assert sqlite_rows(db, "SELECT name FROM big WHERE id = 2") == [("name2Z",)]
        t.keys("q"); assert t.quit() == 0


def test_no_primary_key_is_read_only(home):
    db = sqlite_db(home.file("t.sqlite"), "CREATE TABLE loose (a TEXT); INSERT INTO loose VALUES ('x');")
    t = Nc(home, ["-s", "sqlite://" + db])
    with t:
        t.wait("loose"); t.keys("Enter"); t.wait("x")
        t.keys("Enter")
        t.wait_status("no primary key")
        t.keys("q"); assert t.quit() == 0


def test_delete_with_confirmation(home):
    t, db = open_big(home, 5)
    with t:
        t.keys("Down", "x")
        t.wait("Delete")
        t.keys("Enter")                        # default is Cancel
        t.wait_status(lambda s: "y yes" not in s and "o sort" in s)   # the Delete overlay is gone
        assert "Deleted" not in t.status()
        assert sqlite_rows(db, "SELECT count(*) FROM big") == [(5,)]
        t.keys("x", "y")
        t.wait(at(2, 4))
        assert sqlite_rows(db, "SELECT count(*) FROM big") == [(4,)]
        assert sqlite_rows(db, "SELECT count(*) FROM big WHERE id = 2") == [(0,)]
        t.keys("q"); assert t.quit() == 0


def test_delete_selected_rows(home):
    t, db = open_big(home, 6)
    with t:
        t.keys("Space", "Down", "Down", "Space")   # rows 1 and 3
        t.keys("x")
        t.wait("Delete 2 rows")
        t.keys("y")
        t.wait_status("Deleted 2 rows")
        assert sqlite_rows(db, "SELECT id FROM big ORDER BY id") == [(2,), (4,), (5,), (6,)]
        t.keys("q"); assert t.quit() == 0


def test_add_row(home):
    t, db = open_big(home, 2)
    with t:
        t.keys("+")
        t.wait("Add row")
        t.wait("AI")                           # id is auto-increment: placeholder
        t.keys("Down"); t.type("77")           # v
        t.keys("Down"); t.type("new")          # name
        t.keys("F2")
        t.wait(at(1, 3))
        assert sqlite_rows(db, "SELECT v, name FROM big WHERE id = 3") == [(77, "new")]
        t.keys("q"); assert t.quit() == 0


def test_add_row_error_keeps_form(home):
    db = sqlite_db(home.file("t.sqlite"),
                   "CREATE TABLE req (id INTEGER PRIMARY KEY, name TEXT NOT NULL); INSERT INTO req VALUES (1, 'a');")
    t = Nc(home, ["-s", "sqlite://" + db])
    with t:
        t.wait("req"); t.keys("Enter"); t.wait("a")
        t.keys("+")
        t.wait("required")
        t.keys("F2")                           # name is missing: the database refuses
        t.wait_status("NOT NULL")
        t.wait("Add row")                      # the form stays
        t.keys("Down"); t.type("b"); t.keys("F2")
        t.wait_status("Inserted")
        assert sqlite_rows(db, "SELECT name FROM req WHERE id = 2") == [("b",)]
        t.keys("q"); assert t.quit() == 0


def test_copy_paste(home):
    t, db = open_big(home, 3)
    with t:
        t.keys("Right", "Right", "c")          # copy name1
        t.wait_status("Copied")
        t.keys("Down", "v")                    # paste into row 2
        t.wait_status("Updated")
        assert sqlite_rows(db, "SELECT name FROM big WHERE id = 2") == [("name1",)]
        t.keys("q"); assert t.quit() == 0


def test_paste_inside_inline_edit(home):
    t, db = open_big(home, 3)
    with t:
        t.keys("Right", "Right", "c", "Down", "Enter", "End", "C-u")   # Ctrl+U pastes in an edit
        t.wait("name2name1")
        t.keys("Enter")
        t.wait_status("Updated")
        assert sqlite_rows(db, "SELECT name FROM big WHERE id = 2") == [("name2name1",)]
        t.keys("q"); assert t.quit() == 0
