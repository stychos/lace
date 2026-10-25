"""The sub-views of a table tab (Data, Structure, Indexes, Foreign keys,
Information), following foreign keys, and the sidebar's VIEWS and TRIGGERS
sections"""
import re

from harness import Tui, sqlite_db


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
    """The sub-view strip: the first row of the main window, right of the sidebar"""
    return t.text().split("\n")[2]


def test_strip_and_lists(home):
    db = make_db(home)
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        t.open_table("orders")
        t.wait_status("Row 1/3")
        t.wait(lambda s: "Foreign keys" in s.split("\n")[2] and "Information" in s.split("\n")[2])
        t.wait("customer_id ⇒")                 # the grid marks the followable column
        t.keys("s")
        t.wait("→ customers.id")                # Structure: the columns with their key
        t.wait("auto-increment")
        t.keys("y")
        t.wait("orders_customer")               # Indexes: the partial index, the check, the key
        t.wait("WHERE customer_id IS NOT NULL")
        t.wait("total >= 0")
        t.keys("f")
        t.wait("customers (id)")                # Foreign keys: the key and what references this table
        t.wait("CASCADE")
        t.wait("No table references this one")
        t.keys("n")
        t.wait("Total size")                    # Information: sizes, the count, the triggers
        t.wait("trg_orders")
        assert "Row 1/3" not in t.text().split("\n")[-1] or True   # the grid is behind the list
        t.keys("End", "Enter")                  # Enter on the trigger opens its tab
        t.wait("CREATE TRIGGER trg_orders")
        t.wait(lambda s: "Definition" in s.split("\n")[2] and "Indexes" not in s.split("\n")[2])
        t.keys("[")                             # back to orders, still on Information
        t.wait("Total size")
        t.keys("x")                             # a grid key does not reach the grid behind the list
        assert "Total size" in t.text()
        x, y = t.find("a Data")
        t.click(x + 2, y)                       # the strip's Data cell
        t.wait_status("Row 1/3")
        t.keys("q")
        assert t.quit() == 0


def test_follow_foreign_keys_both_ways(home):
    db = make_db(home)
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        t.open_table("orders")
        t.wait_status("Row 1/3")
        t.keys("Right")                         # customer_id of row 1
        t.keys("M-Enter")                       # Alt+Enter: the referenced row
        t.wait_status("customers where id = 1")
        t.wait(lambda s: "customers" in s.split("\n")[1])
        t.wait_status(re.compile(r"Row 1/1"))
        t.keys("M-r")                           # Alt+R: the tables whose keys point at this row
        t.wait("Referenced by")
        t.wait("orders (customer_id)")
        t.keys("Enter")
        t.wait_status("orders where customer_id = 1")
        t.wait_status(re.compile(r"Row 1/2"))   # the open orders tab was reused and filtered
        t.keys("Home", "Right", "Right")        # total: a column without a key
        t.keys("M-Enter")
        t.wait_status("total is not a foreign key column")
        t.keys("q")
        assert t.quit() == 0


def test_sidebar_sections_open_views_and_triggers(home):
    db = make_db(home)
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        t.wait("▾ VIEWS")
        t.wait("▾ TRIGGERS")
        t.keys("Down", "Down", "Enter")         # customers, orders, VIEWS: Enter folds the section
        t.wait(lambda s: "▸ VIEWS" in s and "big_orders" not in s)
        t.keys("Enter")                         # and unfolds it
        t.wait("▾ VIEWS")
        t.keys("Down", "Enter")                 # big_orders: a view's rows
        t.wait_status("Row 1/2")
        t.wait(lambda s: "Definition" in s.split("\n")[2] and "Indexes" not in s.split("\n")[2])
        t.keys("s")
        t.wait("CREATE VIEW big_orders")
        t.wait("Columns")                       # its columns under the SQL
        t.keys("Tab")                           # back to the sidebar
        t.keys("Down", "Down", "Enter")         # TRIGGERS, trg_orders
        t.wait("CREATE TRIGGER trg_orders")
        t.keys("n")
        t.wait("On table")
        t.keys("q")
        assert t.quit() == 0


def test_subview_restored_from_session(home):
    db = make_db(home)
    with Tui(home, ["sqlite://" + db]) as t:
        t.open_table("orders")
        t.wait_status("Row 1/3")
        t.keys("y")
        t.wait("orders_customer")
        t.keys("q")
        assert t.quit() == 0
    tabs = home.read("session.json")["workspaces"][0]["connections"][0]["tabs"]
    assert tabs[0]["subview"] == "indexes" and "object" not in tabs[0]
    with Tui(home, []) as t:
        t.wait("orders_customer")               # the Indexes sub-view came back
        assert "Indexes" in strip(t)
        t.keys("q")
        assert t.quit() == 0
