"""The structure editors of a table view (Structure, Indexes, Information),
the table actions menu (m) and the lock banner"""
import importlib.util
import os

from harness import sqlite_db, sqlite_rows
from nc import Nc

_spec = importlib.util.spec_from_file_location(
    "nc_test_table", os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_table.py"))
_tt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_tt)
at, sidebar_cols = _tt.at, _tt.sidebar_cols

SLOW = ("SELECT count(*) FROM (WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c "
        "LIMIT 60000000) SELECT x FROM c)")


def make_db(home):
    return sqlite_db(home.file("t.sqlite"), """
        CREATE TABLE customers (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL COLLATE NOCASE,
                                email TEXT UNIQUE);
        CREATE TABLE orders (id INTEGER PRIMARY KEY, customer_id INTEGER REFERENCES customers(id),
                             total REAL);
        INSERT INTO customers (name, email) VALUES ('ann', 'a@x'), ('bob', 'b@x');
        INSERT INTO orders VALUES (1, 1, 5), (2, 1, 20), (3, 2, 30);
    """)


def strip(t):
    return t.text().split("\n")[2][sidebar_cols(t):]


def open_customers(home):
    db = make_db(home)
    t = Nc(home, ["-s", "sqlite://" + db])
    t.wait("orders")
    t.keys("Enter")                      # customers
    t.wait(at(1, 2))
    t.wait(lambda s: "Structure" in s.split("\n")[2])
    return t, db


def test_add_rename_drop_column(home):
    t, db = open_customers(home)
    with t:
        t.keys("s")
        t.wait("auto-increment")
        t.keys("+")                                   # the column form
        t.wait("Add column to customers")
        t.type("note")
        t.keys("Down")
        t.type("TEXT")
        t.keys("Down", "Down")                        # Nullable stays on; Default
        t.type("'x'")
        t.keys("F2")                                  # the confirmation shows the statement
        t.wait('ALTER TABLE "customers" ADD COLUMN "note" TEXT DEFAULT \'x\'')
        t.keys("y")
        t.wait_status("Add column: done")
        t.wait("note")                                # the structure reloaded
        assert any("note" in r[0] for r in sqlite_rows(db, "SELECT sql FROM sqlite_master WHERE name = 'customers'"))
        # rename it: the cursor on its row, r, the prompt, the confirmation
        t.keys("End", "R")
        t.wait("Rename column note")
        t.type("2")
        t.keys("Enter")
        t.wait('RENAME COLUMN "note" TO "note2"')
        t.keys("y")
        t.wait_status("Rename column: done")
        t.wait("note2")
        # drop it
        t.keys("End", "x")
        t.wait("Drop column note2 from customers (2 rows)")
        t.keys("y")
        t.wait_status("Drop column: done")
        t.wait(lambda s: "note2" not in s)
        # a refusal from the server is shown in full: the UNIQUE column cannot go
        # (the cursor clamped to the last row, email, when the list shrank)
        t.keys("x")
        t.wait("Drop column email")
        t.keys("y")
        t.wait_status("cannot drop UNIQUE column")
        # SQLite cannot alter a column: the editor says so
        t.keys("Enter")
        t.wait("Column email")
        t.wait("SQLite cannot alter this")
        t.keys("Escape")


def test_index_editor_and_check(home):
    t, db = open_customers(home)
    with t:
        t.keys("y")
        t.wait("PRIMARY")
        t.keys("+")
        t.wait("New index on customers")
        t.type("ix_name")
        t.keys("Down", "Down", "Enter")               # the columns picker
        t.wait("Enter pick")
        t.keys("Down", "Enter")                       # name
        t.wait("Columns          name")
        t.keys("Down", "Down")                        # the partial condition
        t.type("name <> ''")
        t.keys("F2")
        t.wait("Locks the table while it builds")
        t.keys("y")
        t.wait_status("Create index: done")
        t.wait("ix_name")
        t.wait("WHERE name <> ''")
        assert sqlite_rows(db, "SELECT name FROM sqlite_master WHERE type = 'index' AND name = 'ix_name'")
        t.keys("c")                                   # SQLite: checks only at CREATE TABLE
        t.wait_status("SQLite takes check constraints only at CREATE TABLE")
        t.keys("Home", "x")                           # drop it (the first row)
        t.wait('DROP INDEX "ix_name"')
        t.keys("y")
        t.wait_status("Drop index: done")
        t.wait(lambda s: "ix_name" not in s)
        assert not sqlite_rows(db, "SELECT name FROM sqlite_master WHERE type = 'index' AND name = 'ix_name'")


def test_information_actions(home):
    t, db = open_customers(home)
    with t:
        t.keys("n")
        t.wait("Set the next auto-increment value")
        t.wait("next 3")
        t.keys("End", "Up", "Up", "Enter")            # the counter line (Vacuum, Analyze above it)
        t.wait("Next auto-increment value")
        t.keys("BSpace")
        t.type("10")
        t.keys("Enter")
        t.wait("The next id of customers becomes 10")
        t.keys("y")
        t.wait_status("Set auto-increment: done")
        t.wait("next 10")
        assert sqlite_rows(db, "SELECT seq FROM sqlite_sequence WHERE name = 'customers'")[0][0] == 9


def test_table_menu_new_rename_drop(home):
    t, db = open_customers(home)
    with t:
        t.keys("m")
        t.wait("Table customers")
        t.wait("Truncate table")
        t.keys("Down", "Down", "Down", "Enter")       # New table…
        t.wait("New table")
        t.type("things")
        t.keys("Enter")
        t.wait('CREATE TABLE "things"')
        t.keys("y")
        t.wait_status("New table: done")
        t.wait(lambda s: "things ×" in s.split("\n")[1])      # opened, on Structure
        t.wait(lambda s: "auto-increment" in s)
        t.wait(lambda s: "things" in s.split("\n")[5][:sidebar_cols(t)])   # the sidebar reloaded
        t.keys("m", "Enter")                          # Rename table…
        t.wait("Rename table things")
        t.keys("C-u")                                 # the prompt's text goes
        t.type("stuff")
        t.keys("Enter")
        t.wait('RENAME TO "stuff"')
        t.keys("y")
        t.wait_status("Rename table: done")
        t.wait(lambda s: "stuff ×" in s.split("\n")[1])       # the tab followed
        t.keys("m", "Down", "Down", "Enter")          # Drop table…
        t.wait("Drop table stuff (0 rows)")
        t.keys("y")
        t.wait_status("Drop table: done")
        t.wait(lambda s: "stuff" not in s)            # the tab closed, the sidebar reloaded
        assert not sqlite_rows(db, "SELECT name FROM sqlite_master WHERE name IN ('things', 'stuff')")
        t.keys("m")                                   # Truncate table…: orders reference customers,
        t.wait("Table customers")                     # so SQLite refuses, and the refusal is shown
        t.keys("Down", "Enter")
        t.wait("Delete every row of customers (2 rows)")
        t.keys("y")
        t.wait_status("FOREIGN KEY constraint failed")
        assert sqlite_rows(db, "SELECT COUNT(*) FROM customers")[0][0] == 2
        t.keys("Tab", "Down", "Enter")                # orders, which nothing references
        t.wait("orders ×")
        t.wait(at(1, 3))
        t.keys("m", "Down", "Enter")
        t.wait("Delete every row of orders (3 rows)")
        t.keys("y")
        t.wait_status("Truncate table: done")
        t.wait(at(0, 0))
        assert sqlite_rows(db, "SELECT COUNT(*) FROM orders")[0][0] == 0


def test_lock_banner(home):
    t, db = open_customers(home)
    with t:
        t.keys("p")                                   # a query view with a slow statement
        t.type(SLOW)
        t.keys("C-r")
        t.wait("Esc cancels")                         # the banner on the first content row
        t.wait(lambda s: " s  Esc cancels" in s.split("\n")[2])
        t.keys("Down", "Down")                        # ignored while locked
        assert "Esc cancels" in t.text().split("\n")[2]
        t.keys("[")                                   # tab switching still works
        t.wait(lambda s: "Esc cancels" in s.split("\n")[2])   # the same connection: locked there too
        t.keys("]")
        t.keys("Escape")
        t.wait_status("Cancelled")
        t.wait(lambda s: "Esc cancels" not in s.split("\n")[2])
