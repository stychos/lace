#!/bin/sh
# Lace macOS frontend
# Smoke test: drives the built app with LACE_MACOS_SCRIPT (Sources/Lace/
# ScriptDriver.swift) against a private stdio daemon and a temporary SQLite
# file, in a private HOME so nothing touches the user's files. The window
# appears briefly (accessory activation policy: no Dock icon, no focus).
# First the long run below ("main"), then the scenarios in Tests/smoke/
# (see "The scenarios" further down), each in a HOME of its own.
#
# Usage: [SMOKE=filter] Tests/smoke.sh APP_BINARY [LACED]
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
printf '{"general": {"daemon_spawn_mode": 1, "delete_confirmation": false, "approximate_counts": false, "page_size": 100, "history_mode": 2}, "macos": {"keychain_passwords": false}}\n' \
  > "$tmp/home/Library/Application Support/lace/config.json"

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
# to session.json by quitting as ⌘Q does (AppKit closes the windows
# after applicationWillTerminate; those closes must not empty the file)
# and reopened by the second run below
script="$script;table people;filter name contains a;sort id desc;query SELECT 2;run;terminate"

run_app() {
  HOME="$tmp/home" XDG_RUNTIME_DIR= LACED_LOG="$tmp/laced.log" \
    LACE_MACOS_SHOTS="$tmp/shots" LACE_MACOS_SCRIPT="$1" \
    "$app" 2>>"$tmp/script.log"
}
filter=${SMOKE:-}
status=0
case "main" in *"$filter"*)
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
    # Closing the last window keeps its session for the next launch and, as
    # general.on_last_workspace_close is "quit", ends the app (its status
    # says whether the expectations held; the file is checked below)
    script2="$script2;table people;expect-total 3;expect-cell 0 1 dave;shot restored"
    script2="$script2;expect-session-windows 1;close-window"
    run_app "$script2"
    status=$?
  fi
  if [ $status -eq 0 ]; then
    # The shared files: session.json version 2 with the window's geometry
    # under "macos", config.json's general and macos sections kept
    python3 - "$tmp/home/Library/Application Support/lace" <<'PY' >>"$tmp/script.log" 2>&1 || status=1
import json, sys
d = sys.argv[1]
s = json.load(open(d + "/session.json"))
assert s["version"] == 2, s
ws = s["workspaces"]
assert len(ws) == 1 and "frame" in ws[0]["macos"] and "frame" not in ws[0], ws
c = json.load(open(d + "/config.json"))
assert c["general"]["page_size"] == 100 and c["macos"]["keychain_passwords"] is False, c
PY
    [ $status -eq 0 ] || echo "script: FAIL the shared files (see the traceback in script.log)" >>"$tmp/script.log"
  fi
  grep '^script:' "$tmp/script.log" | grep -v '^script: shot .* window'
  [ $status -eq 0 ] && echo "  ok   main" || echo "  FAIL main"
;; esac

# The scenarios: every Tests/smoke/NAME.script (whose name contains $SMOKE)
# is one run in its own HOME with a stdio daemon and a three-row SQLite
# database ($LACE_SMOKE_DB, table big). Its lines are LACE_MACOS_SCRIPT
# commands joined with ';'; lines starting with '#' are comments or headers:
#   # config: {...}         config.json instead of the default below
#   # expect-exit           the app must end by itself (no quit command)
#   # then: OTHER.script    a second run in the same HOME afterwards
#   # session: {...}        session.json before the run
#   # keychain              the Keychain preference may stay on: the items go to
#                           $HOME/keychain.json, a stand-in for the login
#                           Keychain (which would ask the person at the screen)
# NAME.check, when there is one, is a shell snippet run afterwards with
# CONFIG_DIR set to the HOME's lace directory; it fails the scenario by
# exiting non-zero.
default_cfg='{"general": {"daemon_spawn_mode": 1, "delete_confirmation": false, "approximate_counts": false, "page_size": 100}, "macos": {"keychain_passwords": false}}'
here=$(cd "$(dirname "$0")" && pwd)
header() { sed -n "s/^# $2: *//p" "$1" | head -n 1; }
has() { grep -Eq "^# $2(:|\$)" "$1"; }

# scenario_run SCRIPT HOME [KEYCHAIN_FILE]: one run; prints why it failed, if it did
scenario_run() {
  sfile=$1 shome=$2 kcfile=${3:-}
  text=$(grep -v '^#' "$sfile" | grep -v '^[[:space:]]*$' | sed "s|\$LACE_SMOKE_DB|$shome/t.sqlite|g" | paste -sd ';' -)
  HOME="$shome/home" XDG_RUNTIME_DIR= LACED_LOG="$shome/laced.log" LACE_MACOS_SHOTS="$shome/shots" \
    LACE_MACOS_KEYCHAIN_FILE="$kcfile" LACE_MACOS_SCRIPT="$text" "$app" -s 2>>"$shome/script.log" &
  pid=$!
  waited=0
  while kill -0 $pid 2>/dev/null; do
    if [ $waited -ge 600 ]; then
      kill $pid 2>/dev/null
      echo "timed out (60 s)"
      wait $pid 2>/dev/null
      return
    fi
    sleep 0.1
    waited=$((waited + 1))
  done
  wait $pid
  rc=$?
  if grep -q "unknown command" "$shome/script.log"; then
    grep -m1 "unknown command" "$shome/script.log" | sed "s/^script: //"
  elif [ $rc -ne 0 ]; then
    why=$(grep -m1 "^script: FAIL" "$shome/script.log" | sed "s/^script: FAIL //")
    echo "exit status $rc${why:+: $why}"
  elif ! has "$sfile" expect-exit && ! grep -q "^script: .* checks, 0 failed$" "$shome/script.log"; then
    echo "did not reach quit"
  fi
}

total=0 failed=0
for sfile in "$here"/smoke/*.script; do
  [ -f "$sfile" ] || continue
  name=$(basename "$sfile" .script)
  case "$name" in *_after) continue ;; esac
  case "$name" in *"$filter"*) ;; *) continue ;; esac
  total=$((total + 1))
  shome="$tmp/$name"
  mkdir -p "$shome/home/Library/Application Support/lace" "$shome/shots"
  cfg=$(header "$sfile" config)
  printf '%s\n' "${cfg:-$default_cfg}" >"$shome/home/Library/Application Support/lace/config.json"
  session=$(header "$sfile" session)
  [ -n "$session" ] && printf '%s\n' "$session" >"$shome/home/Library/Application Support/lace/session.json"
  sqlite3 "$shome/t.sqlite" "CREATE TABLE big (id INTEGER PRIMARY KEY, v INTEGER); INSERT INTO big VALUES (1, 10), (2, 20), (3, 30);"
  kc=
  has "$sfile" keychain && kc="$shome/home/keychain.json"
  why=$(scenario_run "$sfile" "$shome" "$kc")
  then=$(header "$sfile" then)
  if [ -z "$why" ] && [ -n "$then" ]; then
    why=$(scenario_run "$here/smoke/$then" "$shome" "$kc")
    [ -n "$why" ] && why="$then: $why"
  fi
  if [ -z "$why" ] && [ -f "$here/smoke/$name.check" ]; then
    out=$(CONFIG_DIR="$shome/home/Library/Application Support/lace" sh "$here/smoke/$name.check" 2>&1) || why="check: $out"
  fi
  if [ -n "$why" ]; then
    failed=$((failed + 1))
    status=1
    echo "  FAIL $name: $why (log: $shome/script.log)"
  else
    echo "  ok   $name"
  fi
done
echo "macos smoke: $total scenarios, $failed failed"
if [ $status -ne 0 ]; then
  echo "smoke: FAILED, kept $tmp (script.log, laced.log, shots/)"
  exit 1
fi
rm -rf "$tmp"
echo "smoke: OK"
