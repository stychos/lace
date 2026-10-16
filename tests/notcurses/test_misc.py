"""Mouse on the chrome, resize and small terminals"""
import importlib.util
import os

from nc import Nc

# test_table.py of this directory, by path: tests/tui has a module of the same name
_spec = importlib.util.spec_from_file_location(
    "nc_test_table", os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_table.py"))
_tt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_tt)
cursor_id, make_db, sidebar_cols = _tt.cursor_id, _tt.make_db, _tt.sidebar_cols


def test_mouse_click_and_wheel(home):
    db = make_db(home, rows=100)
    with Nc(home, ["-s", "sqlite://" + db]) as t:
        t.wait("big"); t.keys("Enter"); t.wait("1 of 100")
        pos = t.find("name5")                  # (x, y) of that cell on screen
        t.click(*pos)
        t.wait("5 of 100")
        t.click(*t.find("name5"), wheel="down")
        assert cursor_id(t) == 5               # the wheel scrolls, the cursor stays
        t.click(*t.find("palette"))            # a hint runs its action
        t.wait("ACTIONS")
        t.keys("Escape")
        t.keys("q"); assert t.quit() == 0


def test_resize_relayouts(home):
    db = make_db(home, rows=100)
    with Nc(home, ["-s", "sqlite://" + db]) as t:
        t.wait("big"); t.keys("Enter"); t.wait("1 of 100")
        t.keys("C-p"); t.wait("ACTIONS")
        t.resize(60, 15)
        t.wait("ACTIONS")                      # the overlay shrank and stayed
        t.resize(30, 8)                        # an overlay open across the floor
        t.wait("too small")
        t.resize(120, 30)
        t.wait("ACTIONS")
        t.keys("Escape")
        t.resize(30, 8)
        t.wait("too small")
        t.resize(120, 30)
        t.wait("1 of 100")
        t.keys("q"); assert t.quit() == 0


def test_header_overflow_collapses_tabs(home):
    db = make_db(home, rows=5)
    with Nc(home, ["-s", "sqlite://" + db, "sqlite://" + db, "sqlite://" + db], cols=40) as t:
        t.wait("t.sqlite")
        assert "│ 1 │ 2" in t.text().splitlines()[0]
        t.keys("q"); assert t.quit() == 0


def test_header_tab_click_switches(home):
    db = make_db(home, rows=5)
    with Nc(home, ["-s", "sqlite://" + db, "sqlite://" + db], cols=100) as t:
        t.wait(lambda s: "│ t.sqlite" in s.split("\n")[0])
        first = t.text().splitlines()[0]
        assert "▸ t.sqlite 2 │ t.sqlite" in first, first
        t.click(first.index("│ ") + 2, 0)      # the other tab's name
        t.wait(lambda s: "▸ t.sqlite │ t.sqlite 2" in s.split("\n")[0])
        t.keys("q"); assert t.quit() == 0


def test_query_divider_drag(home):
    db = make_db(home, rows=5)
    with Nc(home, ["-s", "sqlite://" + db]) as t:
        t.wait("big"); t.keys("p")
        t.type("select 1")
        t.keys("C-r")
        t.wait("1 row")
        _, dy = t.find("─ 1 row")              # the divider carries the summary
        t.drag(sidebar_cols(t) + 20, dy, dy + 4)
        t.wait(lambda s: t.find("─ 1 row") and t.find("─ 1 row")[1] == dy + 4)
        t.drag(sidebar_cols(t) + 20, dy + 4, dy - 2)
        t.wait(lambda s: t.find("─ 1 row") and t.find("─ 1 row")[1] == dy - 2)
        # a drag that crosses the sidebar (on the "big" row) opens nothing and ends at the release
        by = t.find("big")[1]
        t.drag_path([(sidebar_cols(t) + 20, dy - 2), (4, dy - 2), (4, by), (4, dy + 1)])
        t.wait(lambda s: t.find("─ 1 row") and t.find("─ 1 row")[1] == dy + 1)
        assert "select 1" in t.text().splitlines()[0], t.text()
        t.click(sidebar_cols(t) + 20, t.find("select 1")[1])  # a plain click does not resize any more
        assert t.find("─ 1 row")[1] == dy + 1
        t.keys("C-p"); t.wait("ACTIONS")       # the header and status are live again
        t.keys("Escape")
        t.keys("Escape")
        t.keys("q"); assert t.quit() == 0


def test_too_small_q_asks_with_a_transaction(home):
    db = make_db(home, rows=5)
    with Nc(home, ["-s", "sqlite://" + db]) as t:
        t.wait("big"); t.keys("p")
        t.type("BEGIN")
        t.keys("C-r"); t.wait_status("affected")
        t.resize(30, 8)
        t.wait("too small")
        t.keys("q")
        t.wait("Enlarge")                      # not quitting: the confirmation cannot be shown
        t.resize(120, 30)
        t.wait("BEGIN")
        t.keys("Escape", "q")
        t.wait("Quit")
        t.keys("y"); assert t.quit() == 0


def test_too_small_only_q_works(home):
    db = make_db(home, rows=5)
    with Nc(home, ["-s", "sqlite://" + db]) as t:
        t.wait("big")
        t.resize(39, 9)
        t.wait("too small")
        t.keys("C-p", "x")                     # nothing listens
        t.resize(40, 10)
        t.wait(lambda s: "too small" not in s and "big" in s and "ACTIONS" not in s)
        t.resize(30, 8)
        t.wait("too small")
        t.keys("q"); assert t.quit() == 0


def test_click_in_editor_moves_cursor(home):
    db = make_db(home, rows=5)
    with Nc(home, ["-s", "sqlite://" + db]) as t:
        t.wait("big"); t.keys("p")
        t.type("select 1")
        x0 = sidebar_cols(t) + 2               # after the line number gutter
        t.click(x0 + 2, t.find("1 select 1")[1])  # the editor's line (the header and the strip name it too)
        t.type("X")
        t.wait("seXlect 1")
        t.keys("Escape")
        t.keys("q"); assert t.quit() == 0
