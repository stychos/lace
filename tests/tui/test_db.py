"""The TUI against every database (SQLite always; PostgreSQL, MariaDB and
MySQL when the integration runner provides them): browsing, editing,
transactions and recovery as the user sees them"""

import re
import time

from harness import Tui, TuiError, each_db
from test_table import cursor_id

ROWS = 2500


def items(db, table, rows=ROWS):
    """table (id, name, price) with rows rows: id 1..rows, name 'name<id>'"""
    db.exec(f"DROP TABLE IF EXISTS {table}")
    db.exec(f"CREATE TABLE {table} (id INTEGER PRIMARY KEY, name VARCHAR(40), price INTEGER)")
    db.series(table, rows)


def tui(home, db):
    return Tui(home, ["-s", db.url])


def query_tab(t, sql):
    t.keys("p")
    t.wait("SQL Query")
    t.type(sql)
    t.keys("C-r")


def run_sql(t, sql):
    """Replace the (one-line) editor text with sql and run it. Deleting by
    keys, not Ctrl+K, which would go through the system clipboard."""
    t.keys("Escape", "Enter", "End")
    t.tmux("send-keys", "-t", "t", "-N", "300", "BSpace")
    t.type(sql)
    t.keys("C-r")


@each_db
def test_browse_and_edit(home, db):
    items(db, "tt_browse")
    with tui(home, db) as t:
        t.open_table("tt_browse")
        t.wait_status(f"Row 1/{ROWS}")
        t.keys("G")
        t.wait_status(f"Row {ROWS}/{ROWS}")
        assert cursor_id(t) == ROWS
        t.keys("Right", "Enter", "End", *["BSpace"] * 12)
        t.type("edited in tui")
        t.keys("Enter")
        t.wait_status("Cell updated")
    assert db.scalar(f"SELECT name FROM tt_browse WHERE id = {ROWS}") == "edited in tui"


@each_db
def test_sorted_paging(home, db):
    """Sorted by a column with many duplicates, paged down past the loaded
    window: the cursor row is the one SQL puts at that position"""
    items(db, "tt_sorted")
    with tui(home, db) as t:
        t.open_table("tt_sorted")
        t.keys("Right", "Right", "o", "o")  # price DESC (then the key)
        t.wait("▼")
        for _ in range(80):
            t.keys("NPage", delay=0.01)
        t.wait(lambda s: (t.row_number() or 0) > 1200)
        row = t.row_number()
        want = db.scalar(f"SELECT id FROM tt_sorted ORDER BY price DESC, id ASC "
                         f"LIMIT 1 OFFSET {row - 1}")
        assert cursor_id(t) == int(want), (row, want)


@each_db
def test_transaction_status(home, db):
    items(db, "tt_tx", rows=5)
    with tui(home, db) as t:
        t.open_table("tt_tx")
        query_tab(t, "BEGIN")
        t.wait_status("[TX]")
        run_sql(t, "INSERT INTO tt_tx (id, name, price) VALUES (100, 'in tx', 1)")
        t.wait_status("[TX]")
        if db.pg:
            # A failed statement poisons a PostgreSQL transaction
            run_sql(t, "SELECT * FROM tt_no_such_table")
            t.wait_status("[TX FAILED]")
        run_sql(t, "ROLLBACK")
        t.wait(lambda s: "[TX" not in t.status())
    assert db.scalar("SELECT COUNT(*) FROM tt_tx WHERE id = 100") == "0"


@each_db("postgres", "mysql")
def test_lost_connection_in_transaction(home, db):
    """The server kills the TUI's session while a transaction is open: the
    work is gone, statements are refused until ROLLBACK"""
    items(db, "tt_lost", rows=5)
    with tui(home, db) as t:
        t.open_table("tt_lost")
        query_tab(t, db.sql("", "SELECT pg_backend_pid() AS pid", "SELECT CONNECTION_ID() AS pid"))
        t.wait_status(re.compile(r"Row 1/1"))
        m = re.search(r"│\s*(\d+)\s*│", "\n".join(l for l in t.screen() if "│" in l and "pid" not in l))
        if not m:
            raise TuiError("no backend id on screen\n" + t.text())
        pid = m.group(1)
        run_sql(t, "BEGIN")
        t.wait_status("[TX]")
        run_sql(t, "INSERT INTO tt_lost (id, name, price) VALUES (200, 'lost', 1)")
        t.wait_status("[TX]")
        db.exec(db.sql("", f"SELECT pg_terminate_backend({pid})", f"KILL {pid}"))
        run_sql(t, "INSERT INTO tt_lost (id, name, price) VALUES (201, 'after', 1)")
        t.wait("acknowledge")  # "... Run ROLLBACK to acknowledge" (may wrap)
        run_sql(t, "ROLLBACK")
        t.wait(lambda s: "[TX" not in t.status())
        run_sql(t, "SELECT COUNT(*) AS n FROM tt_lost")
        t.wait_status(re.compile(r"Row 1/1"))
    assert db.scalar("SELECT COUNT(*) FROM tt_lost WHERE id >= 200") == "0"


@each_db("postgres")
def test_pg_reads_inside_transaction(home, db):
    """PostgreSQL: the TUI's own reads (pages, counts, a paged form that does
    not parse) run under a savepoint and never break the user's transaction"""
    items(db, "tt_reads")
    with tui(home, db) as t:
        t.open_table("tt_reads")
        query_tab(t, "BEGIN")
        t.wait_status("[TX]")
        # Appending LIMIT after FOR UPDATE is a syntax error: the paged form
        # fails, the statement then runs as written, the transaction lives
        run_sql(t, "SELECT * FROM tt_reads WHERE id <= 3 FOR UPDATE")
        t.wait_status(re.compile(r"Row 1/3"))
        assert "[TX FAILED]" not in t.status()
        t.keys("Escape", "Escape", "[")  # Back to the table tab: page through it
        t.wait_status(f"/{ROWS}")
        t.keys("G")
        t.wait_status(f"Row {ROWS}/{ROWS}")
        assert "[TX FAILED]" not in t.status() and "[TX]" in t.status()
        t.keys("]")
        run_sql(t, "COMMIT")
        t.wait(lambda s: "[TX" not in t.status())


@each_db("postgres")
def test_pg_schema_qualified_table(home, db):
    db.exec('DROP SCHEMA IF EXISTS "TtSales" CASCADE')
    db.exec('CREATE SCHEMA "TtSales"')
    db.exec('CREATE TABLE "TtSales".orders (id INTEGER PRIMARY KEY, name VARCHAR(40), price INTEGER)')
    db.series('"TtSales".orders', 30)
    with tui(home, db) as t:
        t.open_table("TtSales.orders")
        t.wait_status("Row 1/30")
        t.keys("Right", "Enter", "End", *["BSpace"] * 10)
        t.type("qualified")
        t.keys("Enter")
        t.wait_status("Cell updated")
    assert db.scalar('SELECT name FROM "TtSales".orders WHERE id = 1') == "qualified"


@each_db
def test_add_row_with_identity(home, db):
    db.exec("DROP TABLE IF EXISTS tt_add")
    db.exec(db.sql(
        "CREATE TABLE tt_add (id INTEGER PRIMARY KEY, name VARCHAR(40) NOT NULL, note VARCHAR(40) DEFAULT 'dflt')",
        "CREATE TABLE tt_add (id INTEGER GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY, "
        "name VARCHAR(40) NOT NULL, note VARCHAR(40) DEFAULT 'dflt')",
        "CREATE TABLE tt_add (id INT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(40) NOT NULL, "
        "note VARCHAR(40) DEFAULT 'dflt')"))
    db.exec("INSERT INTO tt_add (name) VALUES ('first')")
    with tui(home, db) as t:
        t.open_table("tt_add")
        t.keys("+")
        t.wait("AI")
        t.keys("Right", "Enter")
        t.type("second")
        t.keys("Enter", "F2")
        t.wait_status(re.compile(r"[Ii]nserted|added"))
    assert db.rows("SELECT id, name, note FROM tt_add ORDER BY id") == [
        ("1", "first", "dflt"), ("2", "second", "dflt")]


@each_db
def test_delete_rows(home, db):
    items(db, "tt_del", rows=10)
    with tui(home, db) as t:
        t.open_table("tt_del")
        t.keys("x")
        t.wait("Delete this row?")
        t.keys("y")
        t.wait_status("Row deleted")
        t.keys("Space", "Down", "Space", "x")
        t.wait("Delete 2 selected rows?")
        t.keys("y")
        t.wait_status("2 rows deleted")
    assert db.scalar("SELECT MIN(id) FROM tt_del") == "4"
    assert db.scalar("SELECT COUNT(*) FROM tt_del") == "7"


@each_db("postgres")
def test_pg_writing_cte_runs_once(home, db):
    """A data-modifying CTE that returns rows is not paged: pages and the
    count would each run the UPDATE again"""
    items(db, "tt_cte", rows=3)
    with tui(home, db) as t:
        t.open_table("tt_cte")
        query_tab(t, "WITH u AS (UPDATE tt_cte SET price = price + 100 RETURNING id) "
                     "SELECT * FROM u")
        t.wait(re.compile(r"│ 3\s+│"))
        t.keys("Down", "G")  # Last row: would count, or load a page if paged
        time.sleep(1)  # A background count, had one started
    # Raised by 100 exactly once (series() gives price = id % 97)
    assert db.rows("SELECT id, price FROM tt_cte ORDER BY id") == [
        ("1", "101"), ("2", "102"), ("3", "103")]


@each_db
def test_ctrl_t_refused_inside_transaction(home, db):
    """Ctrl+T in an open transaction would commit (MySQL: BEGIN) or end the
    user's transaction with its own COMMIT/ROLLBACK"""
    items(db, "tt_ctrlt", rows=2)
    with tui(home, db) as t:
        t.open_table("tt_ctrlt")
        query_tab(t, "BEGIN")
        t.wait_status("[TX]")
        run_sql(t, "INSERT INTO tt_ctrlt (id, name, price) VALUES (50, 'x', 1)")
        t.wait_status("[TX]")
        t.keys("C-t")
        t.wait("already open")
        assert "[TX]" in t.status()
        run_sql(t, "ROLLBACK")
        t.wait(lambda s: "[TX" not in t.status())
    assert db.scalar("SELECT COUNT(*) FROM tt_ctrlt WHERE id = 50") == "0"
