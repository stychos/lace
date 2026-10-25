"""The sub-views of a table view (Data, Structure, Indexes, Foreign keys,
Information), following foreign keys, and the sidebar's VIEWS and TRIGGERS
sections"""
import importlib.util
import os

from harness import sqlite_db
from nc import Nc

_spec = importlib.util.spec_from_file_location(
    "nc_test_table", os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_table.py"))
_tt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_tt)
at, sidebar_cols = _tt.at, _tt.sidebar_cols

# Ctrl+Enter and Ctrl+Shift+Enter as a terminal running the kitty keyboard
# protocol reports them (tmux cannot tell them from Enter)
CTRL_ENTER = "\x1b[13;5u"
CTRL_SHIFT_ENTER = "\x1b[13;6u"


def make_db(home):
    return sqlite_db(home.file("t.sqlite"), """
        CREATE TABLE customers (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL COLLATE NOCASE,
                                email TEXT UNIQUE);
        CREATE TABLE orders (id INTEGER PRIMARY KEY,
                             customer_id INTEGER REFERENCES customers(id) ON DELETE CASCADE,
                             total REAL CHECK (total >= 0), note TEXT);
        CREATE INDEX orders_customer ON orders(customer_id) WHERE customer_id IS NOT NULL;
        CREATE VIEW big_orders AS SELECT id, total FROM orders WHERE total > 10;
        CREATE TRIGGER trg_orders AFTER INSERT ON orders BEGIN
          UPDATE customers SET name = name WHERE id = NEW.customer_id; END;
        INSERT INTO customers (name, email) VALUES ('ann', 'a@x'), ('bob', 'b@x');
        INSERT INTO orders VALUES (1, 1, 5, 'x'), (2, 1, 20, 'y'), (3, 2, 30, NULL);
    """)


def strip(t):
    """The sub-view strip: the first row of the view's area"""
    return t.text().split("\n")[2][sidebar_cols(t):]


def open_orders(home):
    db = make_db(home)
    t = Nc(home, ["-s", "sqlite://" + db])
    t.wait("orders")
    t.keys("Down", "Enter")            # customers, orders
    t.wait(at(1, 3))
    t.wait(lambda s: "Foreign keys" in s.split("\n")[2])   # the strip
    return t


def test_strip_and_lists(home):
    with open_orders(home) as t:
        assert "Data" in strip(t) and "Information" in strip(t)
        t.wait("customer_id ⇒")                 # the grid marks the followable column
        t.keys("s")
        t.wait("→ customers.id")                # Structure: the columns with their key
        t.wait("auto-increment")
        t.keys("y")
        t.wait("orders_customer")               # Indexes: the partial index, the check, the key
        t.wait("WHERE customer_id IS NOT NULL")
        t.wait("total >= 0")
        t.wait("PRIMARY")
        t.keys("f")
        t.wait("customers (id)")                # Foreign keys: the key and what references this table
        t.wait("CASCADE")
        t.wait("No table references this one")
        t.keys("n")
        t.wait("Total size")                    # Information: sizes, the count, the triggers
        t.wait("Rows")
        t.wait("trg_orders")
        assert "1 of 3" not in t.text().split("\n")[3]   # the grid is not drawn behind the list
        t.keys("End", "Enter")                  # Enter on the trigger opens its tab
        t.wait("CREATE TRIGGER trg_orders")
        t.wait(lambda s: "Definition" in s.split("\n")[2] and "Indexes" not in s.split("\n")[2])
        t.keys("[")                             # back to orders, still on Information
        t.wait("Total size")
        t.click(sidebar_cols(t) + strip(t).index("Data") + 1, 2)   # the strip's Data cell
        t.wait(at(1, 3))
        t.keys("q"); assert t.quit() == 0


def test_follow_foreign_keys_both_ways(home):
    with open_orders(home) as t:
        t.keys("Right")                         # customer_id of row 1
        t.type(CTRL_ENTER)
        t.wait_status("customers where id = 1")
        t.wait(lambda s: "▸ customers" in s.split("\n")[0] and "id = 1" in s.split("\n")[0])
        t.wait(at(1, 1))
        t.type(CTRL_SHIFT_ENTER)                # the tables whose keys point at this row
        t.wait("Referenced by")
        t.wait("orders (customer_id)")
        t.keys("Enter")
        t.wait_status("orders where customer_id = 1")
        t.wait(lambda s: "customer_id = 1" in s.split("\n")[0])
        t.wait(at(1, 2))                        # the open orders view was reused and filtered
        assert "orders │ customers ×" in t.text().split("\n")[1] or "orders × │ customers" in t.text().split("\n")[1]
        t.keys("Right")                         # total: a column without a key
        t.type(CTRL_ENTER)
        t.wait_status("total is not a foreign key column")
        t.keys("p"); t.type("SELECT customer_id, total FROM orders"); t.keys("C-r")
        t.wait("3 rows")
        t.wait("customer_id ⇒")                 # results that are one table's rows follow too
        t.keys("Escape")
        t.type(CTRL_ENTER)
        t.wait_status("customers where id = 1")
        t.keys("q"); assert t.quit() == 0


def test_sidebar_sections_open_views_and_triggers(home):
    db = make_db(home)
    with Nc(home, ["-s", "sqlite://" + db]) as t:
        t.wait("▾ VIEWS")
        t.wait("▾ TRIGGERS")
        t.keys("Down", "Down", "Down", "Enter")   # customers, orders, VIEWS, big_orders
        t.wait("▸ big_orders")
        t.wait(at(1, 2))                           # a view's rows
        t.wait(lambda s: "Definition" in s.split("\n")[2] and "Data" in s.split("\n")[2]
               and "Indexes" not in s.split("\n")[2])
        t.keys("s")
        t.wait("CREATE VIEW big_orders")
        t.wait("Columns")                          # its columns under the SQL
        t.keys("Tab", "Up", "Enter")               # back in the sidebar, Enter on the VIEWS header folds it
        t.wait(lambda s: "▸ VIEWS" in s and "big_orders" not in "\n".join(l[:sidebar_cols(t)] for l in s.split("\n")[3:]))
        t.keys("Enter")                            # and unfolds it
        t.wait("▾ VIEWS")
        t.keys("Down", "Down", "Down", "Enter")    # big_orders, TRIGGERS, trg_orders
        t.wait("CREATE TRIGGER trg_orders")
        t.keys("n")
        t.wait("On table")
        t.wait(lambda s: "orders" in s.split("\n")[4])
        t.keys("q"); assert t.quit() == 0


def test_subview_restored_from_session(home):
    db = make_db(home)
    with Nc(home, ["sqlite://" + db]) as t:
        t.wait("orders")
        t.keys("Down", "Enter")
        t.wait(at(1, 3))
        t.keys("y")
        t.wait("orders_customer")
        assert t.quit() == 0
    with Nc(home, []) as t:
        t.wait("orders_customer")                  # the Indexes sub-view came back
        assert "Indexes" in strip(t)
        tabs = home.read("session.json")["workspaces"][0]["connections"][0]["tabs"]
        assert tabs[0]["subview"] == "indexes" and "object" not in tabs[0]
        t.keys("q"); assert t.quit() == 0
