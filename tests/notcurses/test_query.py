"""The query view: editor, paged and unpaged statements, scripts, transactions, editable results"""
import importlib.util
import os

from harness import sqlite_exec, sqlite_rows
from nc import Nc

# test_table.py of this directory, by path: tests/tui has a module of the same name
_spec = importlib.util.spec_from_file_location(
    "nc_test_table", os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_table.py"))
_tt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_tt)
at, make_db, cursor_id = _tt.at, _tt.make_db, _tt.cursor_id


def open_query(home, rows=3000):
    db = make_db(home, rows=rows)
    t = Nc(home, ["-s", "sqlite://" + db])
    t.wait("big"); t.keys("p"); t.wait("New query")
    return t, db


def test_select_is_paged_and_counted(home):
    t, db = open_query(home)
    with t:
        t.type("SELECT * FROM big ORDER BY id DESC")
        t.keys("C-r")
        t.wait(at(1, 3000))                    # counted in the background
        t.keys("Escape")                       # editor -> results
        assert cursor_id(t) == 3000
        t.keys("G")
        t.wait(at(3000, 3000))
        assert cursor_id(t) == 1
        t.keys("q"); assert t.quit() == 0


def test_exec_shows_affected(home):
    t, db = open_query(home, 10)
    with t:
        t.type("UPDATE big SET v = v + 1 WHERE id <= 3")
        t.keys("C-r")
        t.wait("3 rows affected")
        assert sqlite_rows(db, "SELECT v FROM big WHERE id = 1") == [(2,)]
        t.keys("Escape", "q"); assert t.quit() == 0


def test_run_all_and_transaction(home):
    t, db = open_query(home, 10)
    with t:
        t.type("UPDATE big SET v = 0 WHERE id = 1;\nSELECT v FROM big WHERE id = 1")
        t.keys("C-a")
        t.wait(at(1, 1))
        t.keys("Escape")
        assert cursor_id(t) == 0
        assert sqlite_rows(db, "SELECT v FROM big WHERE id = 1") == [(0,)]
        t.keys("q"); assert t.quit() == 0


def test_transaction_commits(home):
    t, db = open_query(home, 10)
    with t:
        t.type("UPDATE big SET v = 77 WHERE id = 2;\nSELECT v FROM big WHERE id = 2")
        t.keys("C-t")
        t.wait(at(1, 1))
        assert sqlite_rows(db, "SELECT v FROM big WHERE id = 2") == [(77,)]
        t.keys("Escape", "q"); assert t.quit() == 0


def test_transaction_rolls_back_on_error(home):
    t, db = open_query(home, 10)
    with t:
        t.type("UPDATE big SET v = 99 WHERE id = 2;\nSELECT * FROM nope")
        t.keys("C-t")
        t.wait_status("no such table")
        assert sqlite_rows(db, "SELECT v FROM big WHERE id = 2") == [(2,)]   # rolled back
        t.keys("Escape", "q"); assert t.quit() == 0


def test_error_stops_the_script(home):
    t, db = open_query(home, 10)
    with t:
        t.type("UPDATE big SET v = 5 WHERE id = 1;\nSELECT * FROM nope;\nUPDATE big SET v = 6 WHERE id = 1")
        t.keys("C-a")
        t.wait_status("no such table")
        assert sqlite_rows(db, "SELECT v FROM big WHERE id = 1") == [(5,)]   # first ran, third did not
        t.keys("Escape", "q"); assert t.quit() == 0


def test_statement_at_cursor(home):
    t, db = open_query(home, 10)
    with t:
        t.type("UPDATE big SET v = 11 WHERE id = 1;\nUPDATE big SET v = 22 WHERE id = 2")
        t.keys("Up")                           # on the first statement
        t.keys("C-r")
        t.wait("1 row affected")
        assert sqlite_rows(db, "SELECT v FROM big WHERE id IN (1, 2) ORDER BY id") == [(11,), (2,)]
        t.keys("Escape", "q"); assert t.quit() == 0


def test_kitty_protocol_shifted_keys(home):
    """A terminal speaking the kitty keyboard protocol reports Shift+1 as the
    key '1' with Shift and the text '!' alongside: the text is what gets
    typed, and a Shift+[ with text '{' still runs the '{' hotkey"""
    t, db = open_query(home, 10)
    with t:
        t.type("SELECT 'a' ")
        t.type("\x1b[49;2;33u")        # Shift+1 → '!'
        t.type("\x1b[61;2;43u")        # Shift+= → '+'
        t.type("\x1b[97;2;65u")        # Shift+a → 'A'
        t.type("\x1b[97;65;97u")       # Caps Lock + a (modifier 65 = 1 + caps lock 64) → 'a'
        t.wait("SELECT 'a' !+Aa")
        t.keys("Escape")
        t.type("\x1b[91;2;123u")       # Shift+[ → '{': previous workspace: with one, a message
        t.wait("Only one workspace")
        t.keys("q"); assert t.quit() == 0


def test_results_editable_for_simple_select(home):
    t, db = open_query(home, 5)
    with t:
        t.type("SELECT id, name FROM big")
        t.keys("C-r"); t.wait(at(1, 5)); t.keys("Escape")
        t.keys("Right", "e"); t.keys("End")    # Enter is "back to the editor" here: edit in the cell editor
        for _ in range(10): t.keys("BSpace")
        t.type("via query"); t.keys("Enter")
        t.wait_status("Updated")
        assert sqlite_rows(db, "SELECT name FROM big WHERE id = 1") == [("via query",)]
        t.keys("q"); assert t.quit() == 0


def test_results_of_a_join_are_read_only(home):
    t, db = open_query(home, 5)
    with t:
        t.type("SELECT a.id, b.name FROM big a JOIN big b ON a.id = b.id")
        t.keys("C-r"); t.wait(at(1, 5)); t.keys("Escape")
        t.keys("Right", "e")
        t.wait_status("cannot be edited")
        t.keys("q"); assert t.quit() == 0


def test_enter_and_escape_move_focus(home):
    t, db = open_query(home, 5)
    with t:
        t.type("SELECT id FROM big")
        t.keys("C-r"); t.wait(at(1, 5)); t.keys("Escape")
        t.keys("Down"); t.wait(at(2, 5))
        t.keys("Enter")                        # back to the editor: keys type again
        t.type(" WHERE id < 3")
        t.wait("WHERE id < 3")
        t.keys("C-r"); t.wait(at(1, 2))
        t.keys("Escape", "q"); assert t.quit() == 0


def test_split_resizes(home):
    t, db = open_query(home, 5)
    with t:
        t.type("SELECT id FROM big")
        t.keys("C-r"); t.wait(at(1, 5))
        before = [i for i, l in enumerate(t.text().split("\n")) if "───" in l]
        t.keys("C-Down", "C-Down")
        after = [i for i, l in enumerate(t.text().split("\n")) if "───" in l]
        assert before and after and after[0] == before[0] + 2, (before, after)
        t.keys("Escape", "q"); assert t.quit() == 0


def test_bad_statement_shows_error(home):
    t, db = open_query(home, 5)
    with t:
        t.type("SELEKT 1")
        t.keys("C-r")
        t.wait_status("syntax error")
        t.wait("syntax error")
        t.keys("Escape", "q"); assert t.quit() == 0


def test_join_and_pragma_results_are_drawn(home):
    t, db = open_query(home, 5)
    with t:
        t.type("SELECT a.id, b.name FROM big a JOIN big b ON a.id = b.id ORDER BY a.id")
        t.keys("C-r"); t.wait(at(1, 5))
        t.wait("name3")                        # a cell of the result, not "Loading"
        assert "Loading" not in t.text()
        t.keys("Escape", "Enter")              # results -> back to the editor, cursor at the end
        for _ in range(80): t.keys("BSpace")
        t.type("PRAGMA table_info(big)")
        t.keys("C-r"); t.wait("INTEGER")
        assert "Loading" not in t.text()
        t.keys("Escape", "q"); assert t.quit() == 0


def test_with_insert_runs_exactly_once(home):
    t, db = open_query(home, 5)
    with t:
        t.type("WITH s(n) AS (SELECT 100) INSERT INTO big (id, v, name) SELECT n, n, 'w' FROM s RETURNING id")
        t.keys("C-r")
        t.wait(at(1, 1))
        t.wait("100")
        assert sqlite_rows(db, "SELECT count(*) FROM big") == [(6,)]
        t.keys("Escape", "q"); assert t.quit() == 0


def test_transaction_refused_while_one_is_open(home):
    # the final quit answers the confirmation, since the transaction is still open (see session.quit_asks_with_transaction)
    t, db = open_query(home, 5)
    with t:
        t.type("BEGIN")
        t.keys("C-r"); t.wait_status("affected")
        for _ in range(5): t.keys("BSpace")
        t.type("UPDATE big SET v = 50 WHERE id = 1")
        t.keys("C-t")
        t.wait_status("already open")
        assert sqlite_rows(db, "SELECT v FROM big WHERE id = 1") == [(1,)]
        t.keys("Escape", "q", "y")             # quitting asks while a transaction is open
        assert t.quit() == 0


def test_escape_stops_a_transaction_script(home):
    db = _tt.make_slow_db(home, 200000)
    t = Nc(home, ["-s", "sqlite://" + db])
    with t:
        t.wait("big"); t.keys("p"); t.wait("New query")
        t.type("UPDATE big SET name = 'first' WHERE id = 1;\nSELECT sum(v) FROM big;\n"
               "UPDATE big SET name = 'last' WHERE id = 1")
        t.keys("C-t")
        t.wait(lambda s: "Running" in s)
        t.keys("Escape")
        t.wait_status("rolled back", timeout=30)
        assert sqlite_rows(db, "SELECT name FROM big WHERE id = 1") == [("name1",)]
        t.keys("Escape", "q"); assert t.quit() == 0


def test_long_message_keeps_the_transaction_badge(home):
    # a message too long for the hints drops them, never the [TX] badge
    t, db = open_query(home, 5)
    with t:
        t.type("BEGIN")
        t.keys("C-r"); t.wait_status("affected")
        for _ in range(5): t.keys("BSpace")
        t.type("SELECT * FROM a_table_with_a_name_long_enough_to_push_every_hint_out_of_the_status_line")
        t.keys("C-r")
        t.wait_status("no such table")
        assert "[TX]" in t.status()
        assert "run all" not in t.status()     # the hints made room
        t.keys("Escape", "q", "y")             # quitting asks while a transaction is open
        assert t.quit() == 0
