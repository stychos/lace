"""Query tabs: running statements, paging results, editing results,
transactions"""

import re

from harness import Tui, sqlite_db, sqlite_rows
from test_table import cursor_id, make_db


def open_query(t, sql):
    """A new query tab (starts in the editor) with sql in it"""
    t.wait("TABLES")
    t.keys("p")
    t.wait("runs the statement at the cursor")
    t.type(sql)


def test_run_and_page(home):
    db = make_db(home)
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        open_query(t, "SELECT id, name FROM big ORDER BY id DESC")
        t.keys("C-r")
        t.wait_header(re.compile(r"1 of "))
        t.keys("Escape", "Down")  # Leave the editor, into the results
        assert cursor_id(t) == 3000
        t.keys("G")
        t.wait_header("3,000 of 3,000")
        assert cursor_id(t) == 1


def test_error_shown(home):
    db = make_db(home, rows=3)
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        open_query(t, "SELECT * FROM no_such_table")
        t.keys("C-r")
        t.wait("no such table")


def test_edit_result_cell(home):
    db = make_db(home, rows=5)
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        open_query(t, "SELECT id, name FROM big ORDER BY id")
        t.keys("C-r")
        t.wait_header(re.compile(r"1 of 5"))
        t.keys("Escape", "Down", "Right", "Enter")
        t.keys("End", *["BSpace"] * 8)
        t.type("from query")
        t.keys("Enter")
        t.wait_status("Cell updated")
    assert sqlite_rows(db, "SELECT name FROM big WHERE id = 1") == [("from query",)]


def test_aliased_column_not_written(home):
    db = make_db(home, rows=5)
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        open_query(t, "SELECT id, v AS name FROM big ORDER BY id")
        t.keys("C-r")
        t.wait_header(re.compile(r"1 of 5"))
        t.keys("Escape", "Down", "Right", "Enter")
        t.type("9")
        t.keys("Enter")
        t.wait_status("Not updated")
    assert sqlite_rows(db, "SELECT name FROM big WHERE id = 1") == [("name1",)]


def test_transaction_rolls_back(home):
    db = sqlite_db(home.file("q.sqlite"), "CREATE TABLE t (id INTEGER PRIMARY KEY);")
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        open_query(t, "INSERT INTO t VALUES (1);\nINSERT INTO t VALUES (1);")
        t.keys("C-t")
        t.wait(re.compile(r"[Rr]olled back|ROLLBACK"))
    assert sqlite_rows(db, "SELECT COUNT(*) FROM t") == [(0,)]


def test_run_all(home):
    db = sqlite_db(home.file("q.sqlite"), "CREATE TABLE t (id INTEGER PRIMARY KEY);")
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        open_query(t, "INSERT INTO t VALUES (1);\nINSERT INTO t VALUES (2);\n"
                      "SELECT COUNT(*) AS n FROM t;")
        t.keys("C-a")
        t.wait_header(re.compile(r"1 of 1"))
    assert sqlite_rows(db, "SELECT COUNT(*) FROM t") == [(2,)]
