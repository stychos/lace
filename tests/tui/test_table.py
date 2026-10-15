"""Table tabs: navigation, paging, editing, deleting, sorting"""

import re

from harness import Tui, TuiError, sqlite_db, sqlite_exec, sqlite_rows


def make_db(home, rows=3000):
    return sqlite_db(home.file("t.sqlite"), f"""
        CREATE TABLE big (id INTEGER PRIMARY KEY, v INTEGER, name TEXT);
        WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < {rows})
        INSERT INTO big SELECT n, n, 'name' || n FROM s;
    """)


def cursor_id(tui):
    """The id (first column) of the highlighted row"""
    cells = tui.cursor_cells()
    for c in cells or []:
        if c.isdigit():
            return int(c)
        if c:
            break  # The first non-empty cell is the id
    raise TuiError(f"no cursor row in\n{tui.text()}")


def test_open_and_count(home):
    db = make_db(home)
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        t.open_table("big")
        t.wait_status("Row 1/3000")
        assert cursor_id(t) == 1


def test_last_row_and_page_back(home):
    """Jump to the end, page back across window edges: every position
    still shows its own row (keyset pages)"""
    db = make_db(home)
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        t.open_table("big")
        t.keys("G")
        t.wait_status("Row 3000/3000")
        assert cursor_id(t) == 3000
        for _ in range(60):
            t.keys("PPage", delay=0.02)
        t.wait(lambda s: t.row_number() is not None and t.row_number() < 2000)
        assert cursor_id(t) == t.row_number()
        t.keys("g")
        t.wait_status("Row 1/3000")
        for _ in range(60):
            t.keys("NPage", delay=0.02)
        t.wait(lambda s: (t.row_number() or 0) > 1000)
        assert cursor_id(t) == t.row_number()


def test_goto_row(home):
    db = make_db(home)
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        t.open_table("big")
        t.keys("C-g")
        t.type("2500")
        t.keys("Enter")
        t.wait_status("Row 2500/3000")
        assert cursor_id(t) == 2500


def test_edit_null_empty(home):
    db = make_db(home, rows=10)
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        t.open_table("big")
        t.keys("Right", "Right", "Enter")
        t.keys("End", *["BSpace"] * 10)
        t.type("edited")
        t.keys("Enter")
        t.wait_status("Cell updated")
        t.keys("Down", "n")
        t.wait_status("Cell set to NULL")
        t.keys("Down", "d")
        t.wait_status("Cell set to empty")
        # Opening a cell and confirming it unchanged writes nothing
        t.keys("Down", "Enter")
        t.keys("Enter")
        t.wait_status("No change")
    assert sqlite_rows(db, "SELECT id, name FROM big WHERE id <= 4 ORDER BY id") == [
        (1, "edited"), (2, None), (3, ""), (4, "name4")]


def test_edit_refuses_changed_row(home):
    db = make_db(home, rows=10)
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        t.open_table("big")
        sqlite_exec(db, "UPDATE big SET name = 'elsewhere' WHERE id = 1")
        t.keys("Right", "Right", "Enter")
        t.type("X")
        t.keys("Enter")
        t.wait_status("Not updated")
    assert sqlite_rows(db, "SELECT name FROM big WHERE id = 1") == [("elsewhere",)]


def test_delete_and_bulk_delete(home):
    db = make_db(home, rows=10)
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        t.open_table("big")
        t.keys("x")
        t.wait("Delete this row?")
        t.keys("y")
        t.wait_status("Row deleted")
        t.keys("Space", "Down", "Space", "x")
        t.wait("Delete 2 selected rows?")
        t.keys("y")
        t.wait_status("2 rows deleted")
        # A declined delete deletes nothing
        t.keys("x")
        t.wait("Delete this row?")
        t.keys("n")
        t.wait_status("Delete cancelled")
    assert [r[0] for r in sqlite_rows(db, "SELECT id FROM big ORDER BY id")] == list(range(4, 11))


def test_sort_cycle(home):
    db = make_db(home, rows=50)
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        t.open_table("big")
        t.keys("Right", "o")
        t.wait("▲")
        assert cursor_id(t) == 1
        t.keys("o")
        t.wait("▼")
        t.wait(lambda s: cursor_id(t) == 50)


def test_add_row(home):
    db = make_db(home, rows=3)
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        t.open_table("big")
        t.keys("+")
        t.wait("AI")  # The key is filled in by the database
        t.keys("Right", "Enter")
        t.type("77")
        t.keys("Enter", "Right", "Enter")
        t.type("new one")
        t.keys("Enter", "F2")
        t.wait_status(re.compile(r"[Ii]nserted|Row added|added"))
    assert sqlite_rows(db, "SELECT v, name FROM big WHERE id = 4") == [(77, "new one")]
