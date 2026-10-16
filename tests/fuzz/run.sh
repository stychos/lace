#!/bin/sh
# Fuzz liblace's and the TUI's parsers with libFuzzer (ASan + UBSan) in a
# container, so the host needs no LLVM with libFuzzer (Apple's clang has none).
# Uses the container CLI only: podman when installed, else docker;
# LACE_TEST_DOCKER names another.
#
#   tests/fuzz/run.sh [seconds per target] [target...]   (default 60; all)
#
# Corpora grow in tests/fuzz/build/corpus/<target> (seeded from
# tests/fuzz/corpus); a crash leaves its input in tests/fuzz/build/crashes.
# Reproduce one: run.sh 0 <target> with FUZZ_REPRO=<file under build/crashes>.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
DOCKER=${LACE_TEST_DOCKER:-$(command -v podman >/dev/null 2>&1 && echo podman || echo docker)}
SECONDS_EACH=${1:-60}
[ $# -gt 0 ] && shift
TARGETS=${*:-"sql connstr values tui"}
mkdir -p "$HERE/build/corpus" "$HERE/build/crashes"
exec "$DOCKER" run --rm -v "$ROOT":/repo:ro -v "$HERE/build":/out \
  -e SECONDS_EACH="$SECONDS_EACH" -e TARGETS="$TARGETS" -e FUZZ_REPRO="${FUZZ_REPRO:-}" \
  debian:bookworm sh -c '
set -e
apt-get update -qq >/dev/null
apt-get install -y -qq --no-install-recommends clang libclang-rt-14-dev make pkg-config \
  libncurses-dev libcjson-dev llvm-14 >/dev/null 2>&1
export ASAN_SYMBOLIZER_PATH=/usr/lib/llvm-14/bin/llvm-symbolizer
mkdir /w && cd /repo && tar --exclude="*/build" --exclude="data.sqlite*" -cf - . | (cd /w && tar xf -)
F="-fsanitize=fuzzer-no-link -fno-sanitize-recover=undefined"
make -C /w/liblace SANITIZE=1 EXTRA_CFLAGS="$F" >/dev/null
make -C /w/tui/ncurses SANITIZE=1 EXTRA_CFLAGS="$F" >/dev/null
TUI_OBJS=$(find /w/tui/ncurses/build -name "*.o" ! -name main.o)
CF="-g -O1 -std=c11 -D_GNU_SOURCE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
  -I/w/liblace/include -I/usr/include/cjson -I/w/tui/ncurses/src"
LIBS="/w/liblace/build/liblace.a -lcjson -lm -pthread"
failed=0
for t in $TARGETS; do
  case $t in
    tui) clang $CF /w/tests/fuzz/fuzz_tui.c $TUI_OBJS $LIBS -lncursesw -lmenuw -o /w/fuzz_tui ;;
    values) clang $CF -I/w/laced/src /w/tests/fuzz/fuzz_values.c /w/tests/fuzz/daemon_encode.c \
              /w/laced/src/json.c /w/laced/src/db/db_types.c $LIBS -o /w/fuzz_values ;;
    *) clang $CF /w/tests/fuzz/fuzz_$t.c $LIBS -o /w/fuzz_$t ;;
  esac
  if [ -n "$FUZZ_REPRO" ]; then
    /w/fuzz_$t "/out/crashes/${FUZZ_REPRO#build/crashes/}" || true
    continue
  fi
  mkdir -p /out/corpus/$t
  cp -n /w/tests/fuzz/corpus/$t/* /out/corpus/$t/ 2>/dev/null || true
  echo "== $t (${SECONDS_EACH}s)"
  if ! /w/fuzz_$t -max_total_time=$SECONDS_EACH -max_len=4096 -timeout=10 \
       -artifact_prefix=/out/crashes/$t- -print_final_stats=0 /out/corpus/$t \
       > /out/$t.log 2>&1; then
    failed=1
    grep -A30 -E "invariant broken|ERROR: AddressSanitizer|runtime error|deadly signal|timeout" /out/$t.log | head -60
  fi
  tail -1 /out/$t.log
done
exit $failed
'
