#!/bin/sh
# liblace stress test under ThreadSanitizer (tests/stress/stress.c).
# Builds liblace and laced with SANITIZE=thread in a scratch copy of the
# tree (the normal build stays as it is), runs each scenario for N seconds,
# and fails on a wrong answer, a stall, or any ThreadSanitizer report from
# the test or from a daemon it started.
#
#   tests/stress/run.sh [seconds per scenario] [scenario...]  (default 30;
#                                                  stdio unix detect)
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SECONDS_EACH=${1:-30}
[ $# -gt 0 ] && shift
SCENARIOS=${*:-"stdio unix detect"}

# Short paths: sockets under the work directory must fit sun_path
W=$(mktemp -d /tmp/lst-XXXXXX)
trap 'rm -rf "$W"' EXIT
mkdir "$W/src" "$W/run" "$W/tsan"
(cd "$ROOT" && tar --exclude="*/build" --exclude="data.sqlite*" -cf - liblace laced) |
  (cd "$W/src" && tar xf -)

echo "stress: building liblace and laced with ThreadSanitizer"
make -C "$W/src/liblace" SANITIZE=thread >/dev/null
make -C "$W/src/laced" SANITIZE=thread >/dev/null

CJSON=$(pkg-config --cflags --libs libcjson 2>/dev/null ||
        echo "-I$(brew --prefix 2>/dev/null)/include -L$(brew --prefix 2>/dev/null)/lib -lcjson")
# shellcheck disable=SC2086
${CC:-clang} -std=c11 -D_GNU_SOURCE -g -O1 -fsanitize=thread -Wall -Wextra -Werror \
  -I"$W/src/liblace/include" "$ROOT/tests/stress/stress.c" \
  "$W/src/liblace/build/liblace.a" $CJSON -lpthread -o "$W/stress"

# Reports from every process (spawned daemons write no stderr we can see)
export TSAN_OPTIONS="log_path=$W/tsan/report halt_on_error=0 second_deadlock_stack=1"
status=0
for s in $SCENARIOS; do
  rm -rf "$W/run" && mkdir "$W/run"
  # A hard limit too: a deadlock (e.g. in fork) must fail, not hang
  "$W/stress" "$s" "$W/src/laced/build/laced" "$W/run" "$SECONDS_EACH" &
  pid=$!
  limit=$((SECONDS_EACH + 120))
  while kill -0 $pid 2>/dev/null && [ $limit -gt 0 ]; do
    sleep 1
    limit=$((limit - 1))
  done
  if kill -0 $pid 2>/dev/null; then
    echo "stress: FAIL: scenario $s still running after $((SECONDS_EACH + 120))s; killed"
    kill -9 $pid 2>/dev/null
    status=1
  elif ! wait $pid; then
    status=1
  fi
  pkill -9 -f "$W/src/laced" 2>/dev/null || true
done

if ls "$W"/tsan/report.* >/dev/null 2>&1; then
  echo "stress: ThreadSanitizer reports:"
  cat "$W"/tsan/report.*
  status=1
fi
[ $status -eq 0 ] && echo "stress: all scenarios passed, no ThreadSanitizer reports"
exit $status
