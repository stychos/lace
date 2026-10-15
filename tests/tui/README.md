# TUI tests

The real `lace` binary, driven through a real terminal (tmux), against
SQLite databases the tests create.

```bash
make test-tui                         # needs tmux (skipped without it)
make test-tui TUI_ARGS="query"        # tests whose "module.name" contains "query"
```

Each test gets a private HOME (config, session, daemon log) and a private
tmux server, so nothing touches your own configuration or tmux sessions.
On failure the last screen is printed and the HOME is kept for inspection.

Tests are `test_*` functions in `test_*.py` taking a `harness.Home`:

```python
def test_something(home):
    db = sqlite_db(home.file("x.sqlite"), "CREATE TABLE t (id INTEGER PRIMARY KEY);")
    with Tui(home, ["-s", "sqlite://" + db]) as t:
        t.open_table("t")
        t.keys("x")
        t.wait("Delete this row?")
```

Tests marked `@each_db` run once per database: SQLite always, plus the
PostgreSQL, MariaDB and MySQL servers the integration runner starts:

```bash
make test-integration                     # C tests, then these TUI tests, in containers
python3 tests/integration/run.py --suite tui -- db.   # only the TUI database tests
```

```python
@each_db
def test_something(home, db):          # db: harness.Db
    db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, name VARCHAR(20))")
    with Tui(home, ["-s", db.url]) as t:
        t.open_table("t")
    assert db.rows("SELECT COUNT(*) FROM t") == [("0",)]
```

