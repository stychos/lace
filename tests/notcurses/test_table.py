import re

from harness import TuiError, sqlite_db
from nc import Nc


def make_db(home, rows=3000):
    return sqlite_db(home.file("t.sqlite"), f"""
        CREATE TABLE big (id INTEGER PRIMARY KEY, v INTEGER, name TEXT);
        WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < {rows})
        INSERT INTO big SELECT n, n, 'name' || n FROM s;
    """)


def make_slow_db(home, rows):
    """Like make_db, but sorting by v is slow: v is a virtual column whose
    expression (it depends on id, so SQLite cannot hoist it) costs a few us per row, so ORDER BY v on `rows` rows takes
    seconds on any machine and an Esc after the keypress still catches it"""
    return sqlite_db(home.file("t.sqlite"), f"""
        CREATE TABLE big (id INTEGER PRIMARY KEY,
                          v INTEGER AS (id + length(hex(zeroblob(5000 + id % 3)))) VIRTUAL,
                          name TEXT);
        WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < {rows})
        INSERT INTO big (id, name) SELECT n, 'name' || n FROM s;
    """)


def sidebar_cols(tui):
    """The docked sidebar's width: min(28, cols / 3)"""
    return min(28, tui.cols // 3)


def cursor_id(tui):
    """The id (first column) of the row holding the cursor, found by the
    cursor row's background in a capture with escapes"""
    raw = tui.tmux("capture-pane", "-t", "t", "-p", "-e", capture=True)
    for line in raw.split("\n")[1:tui.rows - 1]:
        if CURSOR_BG.search(line):
            plain = re.sub(r"\x1b\[[0-9;:]*m", "", line)
            plain = plain[sidebar_cols(tui):]
            m = re.match(r"\s*(\d+)", plain)
            if m:
                return int(m.group(1))
    raise TuiError(f"no cursor row in\n{tui.text()}")


# The theme's cursor row background (dark 45475a, light bcc0cc) as tmux
# reports it: true colour, or the nearest of the 256
CURSOR_BG = re.compile(r"\x1b\[(?:[0-9;]*;)?48;(?:2;(?:69;71;90|188;192;204)|5;\d+)m")


def at(n, total):
    """The header's "<row> of <total>" (not the "(1 of 1)" view count at its left)"""
    return re.compile(rf"(?<![(\d,]){re.escape(f'{n:,}')} of {re.escape(f'{total:,}')}(?![\d,)+])")


def current_id(tui):
    try:
        return cursor_id(tui)
    except TuiError:
        return None


def wait_id(tui, want, timeout=10):
    """Wait until the cursor row shows id `want` (a callable for a test): the
    header moves at once, the rows follow when their page lands"""
    ok = want if callable(want) else (lambda i: i == want)
    tui.wait(lambda _s: (lambda i: i is not None and ok(i))(current_id(tui)), timeout)
    return current_id(tui)


def open_big(home, rows, make=make_db):
    db = make(home, rows)
    t = Nc(home, ["-s", "sqlite://" + db])
    t.wait("big")
    t.keys("Enter")            # the first table of the list
    t.wait(at(1, rows))
    return t


def in_sidebar(t, text):
    """A predicate on the screen: text is in the sidebar's list"""
    return lambda s: any(text in l[:sidebar_cols(t)] for l in s.split("\n")[3:])


def test_sidebar_follows_the_view_and_scrolls_long_names(home):
    long = "a_rather_long_table_name_for_the_sidebar"
    sql = "".join(f"CREATE TABLE t{i:02d} (id INTEGER PRIMARY KEY);" for i in range(1, 41))
    db = sqlite_db(home.file("t.sqlite"), f"CREATE TABLE {long} (id INTEGER PRIMARY KEY);" + sql)
    with Nc(home, ["-s", "sqlite://" + db]) as t:
        t.wait("t24")
        assert not in_sidebar(t, "t40")(t.text())      # past the list's end
        t.keys("C-p"); t.type("open table t40"); t.keys("Enter")
        t.wait(in_sidebar(t, "t40"))                   # the opened view's entry scrolls into view...
        raw = t.tmux("capture-pane", "-t", "t", "-p", "-e", capture=True).split("\n")
        line = next(l for l in raw[3:] if "t40" in re.sub(r"\x1b\[[0-9;:]*m", "", l)[:sidebar_cols(t)])
        assert re.search(r"\x1b\[1(?:;|m)", line[:line.index("│")])   # ...highlighted (bold accent)
        t.keys("Tab", "Up")                            # the selection is its own highlight
        t.wait(in_sidebar(t, "t39"))
        raw = t.tmux("capture-pane", "-t", "t", "-p", "-e", capture=True).split("\n")
        line = next(l for l in raw[3:] if "t39" in re.sub(r"\x1b\[[0-9;:]*m", "", l)[:sidebar_cols(t)])
        assert CURSOR_BG.search(line[:line.index("│")])
        t.keys("Escape")
        t.keys("C-p"); t.type("open table a_rather"); t.keys("Enter")
        t.wait(in_sidebar(t, "a_rather_long"))
        t.wait(in_sidebar(t, "for_the_sidebar"), timeout=15)   # the long name scrolls in place
        t.keys("q"); assert t.quit() == 0


def test_grid_shows_rows_and_moves(home):
    with open_big(home, 3000) as t:
        assert wait_id(t, 1) == 1
        t.keys("PageDown")
        assert wait_id(t, lambda i: i > 20) > 20
        t.keys("G")                         # last row: page 2 loads
        t.wait(at(3000, 3000))
        assert wait_id(t, 3000) == 3000
        t.keys("g")
        t.wait(at(1, 3000))
        assert wait_id(t, 1) == 1
        t.keys("C-g"); t.type("1500"); t.keys("Enter")
        t.wait(at(1500, 3000))
        assert wait_id(t, 1500) == 1500
        t.keys("q"); assert t.quit() == 0


def test_sort_and_filter(home):
    with open_big(home, 100) as t:
        t.keys("Right", "o", "o")           # v DESC
        t.wait("↓v")
        assert wait_id(t, 100) == 100       # the sorted page replaces the old rows
        t.wait(at(1, 100))
        t.keys("/")                         # filter on v
        t.wait("Filter")
        t.type("42")                        # value field focused, op = equals
        t.keys("Enter")
        t.wait("v = 42")
        assert wait_id(t, 42) == 42         # the filtered page, not the old rows
        t.wait(at(1, 1))
        t.keys("X")
        assert wait_id(t, 1) == 1
        t.wait(at(1, 100))
        t.wait(lambda s: "v = 42" not in s and "↓v" not in s)
        t.keys("q"); assert t.quit() == 0


def test_structure_and_refresh(home):
    with open_big(home, 5) as t:
        t.keys("s")
        t.wait("INTEGER")                   # the Structure sub-view lists types
        t.wait("name")
        t.keys("a")                         # back to Data
        t.keys("r")
        assert wait_id(t, 1) == 1
        t.wait(at(1, 5))
        t.keys("q"); assert t.quit() == 0


def test_cancel_keeps_rows(home):
    """Esc during a slow page leaves the rows on screen and reports once"""
    with open_big(home, 200000, make_slow_db) as t:
        t.keys("G")
        t.wait(at(200000, 200000))
        t.keys("Right", "o")                # sort by the expensive column
        t.keys("Escape")
        t.wait_status(re.compile(r"[Cc]ancel"), timeout=15)
        assert t.text().count("name") > 1   # rows still drawn
        assert wait_id(t, 200000) == 200000  # the rows of before, cursor where it was
        assert "\u2191v" not in t.text()     # the sort that was cancelled is given up
        t.wait(at(200000, 200000))
        t.keys("q"); assert t.quit() == 0


def test_filter_operator_and_sort_picker(home):
    with open_big(home, 100) as t:
        t.keys("Right", "Right", "/")       # filter on name
        t.wait("Filter")
        for _ in range(7):                  # = != > >= < <= in contains
            t.keys("M-Right")
        t.wait("contains")
        t.type("9")
        t.keys("Enter")
        t.wait("name contains '9'")
        t.wait(at(1, 19))
        t.keys("C-p"); t.type("sort by"); t.keys("Enter")
        t.wait("Sort by column")
        t.keys("Down", "Enter", "Enter")    # v: asc, then desc
        t.wait("↓")
        t.keys("Escape")                    # closing applies
        t.wait("↓v")
        assert wait_id(t, 99) == 99         # v descending among the matches
        t.keys("C-g"); t.type("0"); t.keys("Enter")
        t.wait_status("Not a row number")
        t.keys("q"); assert t.quit() == 0


def test_mouse_click_sort_and_wheel(home):
    with open_big(home, 3000) as t:
        x, y = t.find("name3")              # a cell of the row with id 3
        t.click(x, y)
        assert wait_id(t, 3) == 3
        x, hy = t.find("v ")                # the header of v: sort it
        t.click(x, hy)
        t.wait("↑v")
        t.click(x, hy)
        t.wait("↓v")
        assert wait_id(t, 3000) == 3000     # v descending: the last id on top
        t.click(x, hy)                      # off again
        t.wait(lambda s: "↓v" not in s and "↑v" not in s)
        assert wait_id(t, 1) == 1
        t.click(sidebar_cols(t) + 10, 5, wheel="down")
        t.wait(lambda s: "name1\n" not in s and "name4" in s)   # scrolled by 3 rows
        t.keys("q"); assert t.quit() == 0
