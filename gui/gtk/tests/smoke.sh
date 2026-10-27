#!/bin/sh
# Lace GTK frontend: headless smoke scenarios
#
#   sh tests/smoke.sh [filter]     (or make -C gui/gtk smoke)
#
# Every tests/smoke/NAME.script (whose name contains the filter) is one run
# of build/lace-gtk under Xvfb and dbus-run-session, with a private HOME, a
# stdio daemon and a three-row SQLite database ($LACE_SMOKE_DB). The
# script's lines are LACE_GTK_SCRIPT commands (src/debug_script.c), joined
# with ';'; lines starting with '#' are comments or headers:
#
#   # args: -s              arguments for lace-gtk
#   # config: {...}         config.json instead of the default
#   # fixture-conns         connections.json: "Local" (the database);
#   # fixture-conns: pg     ... and "Alpha", and "Beta" in folder "Work" (PostgreSQL, never reached)
#   # expect-exit           the app must end by itself (no quit command)
#   # then: OTHER.script    a second run in the same HOME afterwards
#
# A scenario fails when lace-gtk exits non-zero (an expect-* command failed:
# status 3; a timeout: 124), its log has an unknown command or a GLib/GTK
# CRITICAL (a bad pointer, a failed assertion), or NAME.check
# (a shell snippet run with HOME_DIR set) exits non-zero.
set -u
here=$(cd "$(dirname "$0")" && pwd)
gtk=$(dirname "$here")
bin="$gtk/build/lace-gtk"
filter=${1:-}
for tool in Xvfb dbus-run-session python3; do
  command -v "$tool" >/dev/null 2>&1 || { echo "gtk smoke: skipped ($tool not found)"; exit 0; }
done
[ -x "$bin" ] || { echo "gtk smoke: $bin not built"; exit 1; }

fifo=$(mktemp -u)
mkfifo "$fifo"
Xvfb -displayfd 3 -screen 0 1400x900x24 3>"$fifo" >/dev/null 2>&1 &
xvfb=$!
read -r display <"$fifo"
rm -f "$fifo"
trap 'kill $xvfb 2>/dev/null' EXIT INT TERM

header() { sed -n "s/^# $2: *//p" "$1" | head -n 1; }
has() { grep -q "^# $2\$\|^# $2:" "$1"; }

# run SCRIPT HOME: one lace-gtk run; prints why it failed, if it did
run_one() {
  script=$1 home=$2
  args=$(header "$script" args)
  text=$(grep -v '^#' "$script" | grep -v '^[[:space:]]*$' | sed "s|\$LACE_SMOKE_DB|$home/t.sqlite|g" | paste -sd ';' -)
  limit=60
  env -i PATH="$PATH" HOME="$home" XDG_CONFIG_HOME="$home/.config" XDG_DATA_HOME="$home/.local/share" \
    XDG_RUNTIME_DIR="$home/run" LACED_LOG="$home/laced.log" DISPLAY=":$display" GDK_BACKEND=x11 \
    GSK_RENDERER=cairo LIBGL_ALWAYS_SOFTWARE=1 GTK_A11Y=none MALLOC_PERTURB_=165 LACE_GTK_SHOTS="$home" LACE_GTK_SCRIPT="$text" \
    timeout $limit dbus-run-session -- "$bin" $args >"$home/log" 2>&1
  status=$?
  if grep -q "unknown or inapplicable command" "$home/log"; then
    grep -m1 "unknown or inapplicable command" "$home/log" | sed "s/.*script: //"
  elif grep -q -- "-CRITICAL \*\*" "$home/log"; then
    grep -m1 -- "-CRITICAL \*\*" "$home/log" | sed "s/.*CRITICAL \*\*: [0-9:.]* //"
  elif [ $status -ne 0 ]; then
    reason=$(grep -m1 "expectation failed" "$home/log" | sed "s/.*script: //")
    echo "exit status $status${reason:+: $reason}"
  fi
}

total=0 failed=0
for script in "$here"/smoke/*.script; do
  name=$(basename "$script" .script)
  case "$name" in *_after) continue ;; esac
  case "$name" in *"$filter"*) ;; *) continue ;; esac
  total=$((total + 1))
  home=$(mktemp -d "${TMPDIR:-/tmp}/lace-gtk-smoke-XXXXXX")
  mkdir -p "$home/.config/lace" "$home/.local/share" "$home/run"
  chmod 700 "$home/run"
  cfg=$(header "$script" config)
  printf '%s\n' "${cfg:-{\"general\": {\"daemon_spawn_mode\": 1, \"restore_session\": true}}}" \
    >"$home/.config/lace/config.json"
  python3 - "$home/t.sqlite" <<'PY'
import sqlite3, sys
c = sqlite3.connect(sys.argv[1])
c.execute("CREATE TABLE big (id INTEGER PRIMARY KEY, v INTEGER)")
c.executemany("INSERT INTO big VALUES (?, ?)", [(1, 10), (2, 20), (3, 30)])
c.commit()
PY
  if has "$script" fixture-conns; then
    python3 - "$home" "$(header "$script" fixture-conns)" <<'PY'
import json, sys
home, kind = sys.argv[1], sys.argv[2]
def conn(cid, name, driver, **kw):
    c = {"type": "connection", "id": cid, "name": name, "driver": driver, "host": "", "port": 0,
         "database": "", "user": "", "password": "", "save_password": False}
    c.update(kw)
    return c
kids = [conn("11111111-0000-4000-8000-000000000001", "Local", "sqlite", database=home + "/t.sqlite")]
if kind == "pg":
    kids += [conn("11111111-0000-4000-8000-00000000000a", "Alpha", "postgres", host="h1", port=5432,
                  database="shop", user="app"),
             {"type": "folder", "name": "Work", "expanded": True, "children": [
                 conn("11111111-0000-4000-8000-00000000000b", "Beta", "postgres", host="h2", port=5432,
                      database="shop", user="app")]}]
json.dump({"root": {"type": "folder", "name": "Connections", "expanded": True, "children": kids}},
          open(home + "/.config/lace/connections.json", "w"))
PY
  fi
  why=$(run_one "$script" "$home")
  then=$(header "$script" then)
  if [ -z "$why" ] && [ -n "$then" ]; then
    why=$(run_one "$here/smoke/$then" "$home")
    [ -n "$why" ] && why="$then: $why"
  fi
  if [ -z "$why" ] && [ -f "$here/smoke/$name.check" ]; then
    out=$(HOME_DIR="$home" sh "$here/smoke/$name.check" 2>&1) || why="check: $out"
  fi
  pkill -x laced -U "$(id -u)" 2>/dev/null >/dev/null; true
  if [ -n "$why" ]; then
    failed=$((failed + 1))
    echo "  FAIL $name: $why (kept $home)"
  else
    echo "  ok   $name"
    rm -rf "$home"
  fi
done
echo "gtk smoke: $total scenarios, $failed failed"
[ $failed -eq 0 ]
