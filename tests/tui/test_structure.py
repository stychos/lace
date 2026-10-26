"""The structure editors of a table tab (Structure, Indexes, Information),
the table actions menu (m) and the refusals a driver or the server gives"""
import re

from harness import Tui, sqlite_db, sqlite_rows


def make_db(home):
    return sqlite_db(home.file("t.sqlite"), """
        CREATE TABLE customers (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL COLLATE NOCASE,
                                email TEXT UNIQUE);
        CREATE TABLE orders (id INTEGER PRIMARY KEY, customer_id INTEGER REFERENCES customers(id),
                             total REAL);
        INSERT INTO customers (name, email) VALUES ('ann', 'a@x'), ('bob', 'b@x');
        INSERT INTO orders VALUES (1, 1, 5), (2, 1, 20), (3, 2, 30);
    """)


def test_add_rename_drop_column(home):
    db = make_db(home)
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        t.open_table("customers")
        t.wait_status("Row 1/2")
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
        t.keys("End", "R")                            # rename it: the prompt, the confirmation
        t.wait("Rename column note")
        t.type("2")
        t.keys("Enter")
        t.wait('RENAME COLUMN "note" TO "note2"')
        t.keys("y")
        t.wait_status("Rename column: done")
        t.wait("note2")
        t.keys("End", "x")                            # drop it
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
        t.keys("q")
        assert t.quit() == 0


def test_index_editor_and_check(home):
    db = make_db(home)
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        t.open_table("customers")
        t.wait_status("Row 1/2")
        t.keys("y")
        t.wait("PRIMARY")
        t.keys("+")
        t.wait("New index on customers")
        t.type("ix_name")
        t.keys("Down", "Down", "Enter")               # the columns picker
        t.wait("Columns")
        t.keys("Down", "Enter")                       # name
        t.wait(re.compile(r"Columns\s+name"))
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
        t.keys("q")
        assert t.quit() == 0


def test_information_actions(home):
    db = make_db(home)
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        t.open_table("customers")
        t.wait_status("Row 1/2")
        t.keys("n")
        t.wait("Set the next auto-increment value")
        t.wait("next 3")
        t.keys("End", "Up", "Up", "Enter")            # the counter line (Vacuum, Analyze above it)
        t.wait("Next auto-increment value")
        t.keys("C-u")
        t.type("10")
        t.keys("Enter")
        t.wait("The next id of customers becomes 10")
        t.keys("y")
        t.wait_status("Set auto-increment: done")
        t.wait("next 10")
        assert sqlite_rows(db, "SELECT seq FROM sqlite_sequence WHERE name = 'customers'")[0][0] == 9
        t.keys("q")
        assert t.quit() == 0


def test_table_menu_new_rename_drop(home):
    db = make_db(home)
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        t.open_table("customers")
        t.wait_status("Row 1/2")
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
        t.wait(lambda s: "things" in s.split("\n")[1])        # opened, on Structure
        t.wait("auto-increment")
        t.wait(lambda s: any(l.startswith("│ things") for l in s.split("\n")))   # the sidebar reloaded
        t.keys("m")                                   # Rename table…
        t.wait("Table things")
        t.keys("Enter")
        t.wait("Rename table things")
        t.keys("C-u")
        t.type("stuff")
        t.keys("Enter")
        t.wait('RENAME TO "stuff"')
        t.keys("y")
        t.wait_status("Rename table: done")
        t.wait(lambda s: "stuff" in s.split("\n")[1])         # the tab followed
        t.keys("m")
        t.wait("Table stuff")
        t.keys("Down", "Down", "Enter")               # Drop table…
        t.wait("Drop table stuff (0 rows)")
        t.keys("y")
        t.wait_status("Drop table: done")
        t.wait(lambda s: "stuff" not in s)            # the tab closed, the sidebar reloaded
        assert not sqlite_rows(db, "SELECT name FROM sqlite_master WHERE name IN ('things', 'stuff')")
        t.keys("Tab")                                 # the focus ring: back to the sidebar
        t.open_table("orders")                        # orders, which nothing references
        t.wait_status("Row 1/3")
        t.keys("m")
        t.wait("Table orders")
        t.keys("Down", "Enter")                       # Truncate table…
        t.wait("Delete every row of orders (3 rows)")
        t.keys("y")
        t.wait_status("Truncate table: done")
        t.wait_status("Row 0/0")
        assert sqlite_rows(db, "SELECT COUNT(*) FROM orders")[0][0] == 0
        t.keys("q")
        assert t.quit() == 0


def test_sqlite_refusals(home):
    db = make_db(home)
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        t.open_table("orders")
        t.wait_status("Row 1/3")
        t.keys("f")                                   # Foreign keys: SQLite cannot add or drop one
        t.wait("customers (id)")
        t.keys("+")
        t.wait_status("SQLite cannot add or drop a foreign key")
        t.keys("x")
        t.wait_status("SQLite cannot add or drop a foreign key")
        t.keys("s", "P")                              # the key editor: disabled with the reason
        t.wait("Primary key of orders")
        t.wait("SQLite cannot alter this")
        t.keys("Escape")
        t.keys("Space")                               # move: MySQL only
        t.wait_status("Only MySQL can reorder columns")
        t.keys("q")
        assert t.quit() == 0
