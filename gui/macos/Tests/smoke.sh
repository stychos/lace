#!/bin/sh
# Lace macOS frontend
# Smoke test: drives the built app with LACE_MACOS_SCRIPT (Sources/Lace/
# ScriptDriver.swift) against a private stdio daemon and a temporary SQLite
# file, in a private HOME so nothing touches the user's files. The window
# appears briefly (accessory activation policy: no Dock icon, no focus).
#
# Usage: Tests/smoke.sh APP_BINARY [LACED]
# Exit 1 when an expectation failed; the temporary directory is kept then.
#
# (c) iloveyou, 2025. MIT License.
# https://github.com/stychos/lace

set -u
app=${1:?app binary}
laced=${2:-}
if [ -n "$laced" ]; then
  export LACE_TEST_LACED="$laced"
fi

# A short path: a Unix socket path must fit sun_path, and $TMPDIR on macOS is long
tmp=$(mktemp -d /tmp/lace-smoke.XXXXXX) || exit 1
mkdir -p "$tmp/home/Library/Application Support/lace" "$tmp/shots"
printf '{"daemon_spawn_mode": 1, "confirm_delete": false, "approximate_counts": false, "page_size": 100}\n' \
  > "$tmp/home/Library/Application Support/lace/macos.json"

db="$tmp/smoke.sqlite"
script="connect sqlite://$db?create=1"
script="$script;query CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT NOT NULL, score REAL, note TEXT)\\; INSERT INTO people (name, score, note) VALUES ('alice', 1.5, NULL), ('bob', 2.5, 'x'), ('carol', NULL, 'y')"
script="$script;run-all;expect-status 2 statements;expect-recent 1 smoke.sqlite"
# Keyset paging (page size 100): a jump reads by OFFSET, the pages next to
# a loaded one by key, forward and backward, under a sort too; the cell
# values prove both kinds of page agree on positions
script="$script;query CREATE TABLE big (id INTEGER PRIMARY KEY, n INTEGER NOT NULL)\; WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < 350) INSERT INTO big (n) SELECT x * 10 FROM c;run-all"
script="$script;table big;expect-total 350;expect-paging offset;expect-cell 0 1 10"
script="$script;expect-cell 300 1 3010;expect-paging offset;expect-cell 250 1 2510;expect-paging keyset-backward;expect-cell 101 1 1020;expect-paging keyset"
script="$script;sort n desc;expect-cell 0 1 3500;expect-cell 150 1 2000;expect-paging keyset;expect-cell 349 1 10;expect-paging offset"
# Query results of a plain SELECT are editable (verified by key)
script="$script;query SELECT * FROM people ORDER BY id;run;expect-total 3;expect-cell 1 1 bob"
script="$script;cell 1 1;edit bobby;expect-cell 1 1 bobby"
# The table tab: edits, NULL, sort, filter, delete
script="$script;table people;expect-total 3;expect-cell 1 1 bobby;expect-cell 2 2 NULL"
script="$script;cell 0 3;edit hello;expect-cell 0 3 hello;cell 0 3;null;expect-cell 0 3 NULL"
script="$script;sort name desc;expect-cell 0 1 carol;sort id"
script="$script;filter score gt 2;expect-total 1;expect-cell 0 1 bobby;shot grid"
script="$script;cell 0 0;delete;expect-total 0"
# Add row: typed values by type, NULL explicit, Empty explicit; the filter
# (score > 2) still applies to the reload
script="$script;add-row name=dave score=3.25 note=null;expect-total 1;expect-cell 0 1 dave;expect-cell 0 3 NULL"
script="$script;add-row name=empty score=4;expect-total 2;expect-cell 1 2 4"
# Every statement that ran and every verified write is in the history
script="$script;expect-history 11 INSERT INTO"
# Counts, several statements, an error
script="$script;query SELECT count(*) AS n FROM people;run;expect-cell 0 0 4"
script="$script;query SELECT 1\\; SELECT 2;run-all;expect-cell 0 0 2"
script="$script;query SELEC nope;run;expect-status Failed;shot query"
# A saved connection (connections.json in the private HOME): its history is
# kept in history/<id>.json and comes back on the next connect
script="$script;saved-add demo sqlite://$db;expect-saved 1;connect-saved demo;expect-recent 1;query SELECT 1;run;expect-history 1 SELECT 1"
script="$script;close-connection;connect-saved demo;expect-history 1 SELECT 1;shot welcome-saved"
# The sub-views of a table tab, the sidebar's sections, following keys:
# orders.customer_id references customers; a view and a trigger list under
# the tables and open with their definitions (made with the sqlite3 CLI: a
# trigger body holds ';', which the query tab's script splitter stops at)
sqlite3 "$db" "CREATE TABLE customers (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL COLLATE NOCASE);
CREATE TABLE orders (id INTEGER PRIMARY KEY, customer_id INTEGER REFERENCES customers(id) ON DELETE CASCADE, total REAL CHECK (total >= 0));
CREATE INDEX orders_customer ON orders(customer_id) WHERE customer_id IS NOT NULL;
CREATE VIEW big_orders AS SELECT id, total FROM orders WHERE total > 10;
CREATE TRIGGER trg_orders AFTER INSERT ON orders BEGIN UPDATE customers SET name = name WHERE id = NEW.customer_id; END;
INSERT INTO customers (name) VALUES ('ann'), ('bob');
INSERT INTO orders VALUES (1, 1, 5), (2, 1, 20), (3, 2, 30);" || exit 1
script="$script;table orders;expect-total 3;expect-subview data"
script="$script;subview structure;expect-subview structure;expect-row customer_id;expect-row → customers.id"
script="$script;subview indexes;expect-row orders_customer;expect-row WHERE customer_id IS NOT NULL;expect-row total >= 0"
script="$script;subview foreign_keys;expect-row customers (id);expect-row CASCADE;expect-row No table references this one"
script="$script;subview information;expect-row Rows;expect-row Total size;expect-row trg_orders"
script="$script;subview data;cell 0 1;follow;expect-total 1;expect-cell 0 1 ann;cell 0 0;referenced-by;expect-total 2;expect-cell 1 2 20"
script="$script;expect-sidebar ▾ VIEWS;expect-sidebar big_orders;expect-sidebar ▾ TRIGGERS;expect-sidebar trg_orders"
script="$script;object view big_orders;expect-total 2;subview structure;expect-row CREATE VIEW big_orders;expect-row Columns"
script="$script;object trigger trg_orders;expect-subview definition;expect-row CREATE TRIGGER trg_orders;subview information;expect-row On table"
script="$script;sidebar-toggle view;expect-sidebar ▸ VIEWS;sidebar-toggle view;expect-sidebar ▾ VIEWS;shot subviews"
# The structure editors (every change through DdlRunner: confirmed by the
# script, run, the tab reloaded): a column added through the editor sheet,
# renamed through the prompt, dropped; an index created and dropped; the
# counter set from Information; the table actions: a new table opened on
# Structure, renamed (the tab follows), dropped (the tab closes); a
# truncate; and the lock banner over a slow statement, cancelled
script="$script;table customers;subview structure;ddl action add;ddl-field name note;ddl-field type TEXT;ddl-field default 'x';ddl-save"
script="$script;expect-row note;expect-row 'x';select-row note;answer memo;ddl action rename;expect-row memo;expect-no-row note"
script="$script;select-row memo;ddl action drop;expect-no-row memo;shot structure"
script="$script;subview indexes;ddl action add;ddl-field name ix_cust;ddl-field columns name;ddl-field where name <> '';ddl-save"
script="$script;expect-row ix_cust;expect-row WHERE name <> '';select-row ix_cust;ddl action drop;expect-no-row ix_cust"
script="$script;subview information;expect-row Set the next auto-increment;ddl autoinc 10;expect-row next 10"
script="$script;ddl new-table things;expect-table things;expect-subview structure;expect-row auto-increment;expect-sidebar things"
script="$script;ddl rename-table stuff;expect-table stuff;expect-sidebar stuff;expect-no-sidebar things"
script="$script;ddl drop;expect-no-sidebar stuff;expect-tabs 4"
script="$script;table orders;ddl truncate;expect-total 0"
script="$script;query SELECT count(*) FROM (WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c LIMIT 80000000) SELECT x FROM c)"
script="$script;run-async;expect-banner;shot banner;cancel;expect-no-banner;expect-status Cancelled;close-tab"
# The session: a table tab with a filter and a sort, a query tab, written
# to macos-session.json by quitting as ⌘Q does (AppKit closes the windows
# after applicationWillTerminate; those closes must not empty the file)
# and reopened by the second run below
script="$script;table people;filter name contains a;sort id desc;query SELECT 2;run;terminate"

run_app() {
  HOME="$tmp/home" XDG_RUNTIME_DIR= LACED_LOG="$tmp/laced.log" \
    LACE_MACOS_SHOTS="$tmp/shots" LACE_MACOS_SCRIPT="$1" \
    "$app" 2>>"$tmp/script.log"
}
run_app "$script"
status=$?
if [ $status -eq 0 ]; then
  # Second run: the session comes back (the first, unsaved connection and
  # the saved one with its two tabs, the table's filter and sort), and the
  # saved connection's history with it
  # (the ten structure changes of the first run are in the history too; the
  # saved connection's six tabs: orders, customers, big_orders, the trigger
  # on its Information, people, and the query)
  script2="restore-session;expect-connections 2;expect-tabs 6;expect-history 12 SELECT 2"
  script2="$script2;close-tab;expect-tabs 5;object trigger trg_orders;expect-subview information;expect-row On table"
  # Closing the last window keeps its session for the next launch
  script2="$script2;table people;expect-total 3;expect-cell 0 1 dave;shot restored"
  script2="$script2;expect-session-windows 1;close-window;expect-session-windows 1;quit"
  run_app "$script2"
  status=$?
fi
grep '^script:' "$tmp/script.log" | grep -v '^script: shot .* window'
if [ $status -ne 0 ]; then
  echo "smoke: FAILED, kept $tmp (script.log, laced.log, shots/)"
  exit 1
fi
rm -rf "$tmp"
echo "smoke: OK"
