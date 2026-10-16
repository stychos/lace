#!/bin/sh
# Windows check without Windows: cross-build liblace with MinGW-w64, run its
# unit tests and a TCP smoke test against a Linux laced under Wine, all in a
# container (amd64: Wine runs x86-64 Windows programs). Uses the container
# CLI only: podman when installed, else docker; LACE_TEST_DOCKER names another.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DOCKER=${LACE_TEST_DOCKER:-$(command -v podman >/dev/null 2>&1 && echo podman || echo docker)}
exec "$DOCKER" run --rm --platform linux/amd64 -v "$ROOT":/repo:ro debian:bookworm sh -c '
set -e
apt-get update -qq >/dev/null
apt-get install -y -qq --no-install-recommends gcc-mingw-w64-x86-64 wine64 make clang \
  pkg-config libncurses-dev libsqlite3-dev libpq-dev libmariadb-dev libcjson-dev \
  git ca-certificates >/dev/null 2>&1
export WINEDEBUG=-all
git clone -q --depth 1 --branch v1.7.18 https://github.com/DaveGamble/cJSON /cjson 2>/dev/null
mkdir -p /mingw/cjson && cp /cjson/cJSON.h /mingw/cjson/
x86_64-w64-mingw32-gcc -c /cjson/cJSON.c -o /mingw/cJSON.o
x86_64-w64-mingw32-ar rcs /mingw/libcjson.a /mingw/cJSON.o

mkdir /w && cd /repo && tar --exclude="*/build" --exclude="data.sqlite*" -cf - . | (cd /w && tar xf -)

# The daemon, for Linux
make -C /w laced >/dev/null

# liblace and its unit tests, for Windows
cp -r /w/liblace /w/liblace-win && rm -rf /w/liblace-win/build
M="OS=Windows_NT CC=x86_64-w64-mingw32-gcc AR=x86_64-w64-mingw32-ar CJSON_CFLAGS=-I/mingw CJSON_LDFLAGS=-L/mingw EXTRA_CFLAGS=-Werror"
make -C /w/liblace-win $M both build/test_liblace >/dev/null
/usr/lib/wine/wine64 /w/liblace-win/build/test_liblace.exe 2>/dev/null

# The smoke test: Windows client, Linux daemon, TCP with a token
x86_64-w64-mingw32-gcc -std=c11 -Wall -Wextra -Werror -I/w/liblace/include -I/mingw \
  /w/tests/windows/smoke.c /w/liblace-win/build/liblace.a -o /w/smoke.exe \
  -L/mingw -lcjson -lws2_32 -lpthread -static
/w/laced/build/laced --tcp 127.0.0.1:7455 --token-file /w/token --quiet --log /w/laced.log &
for i in $(seq 1 100); do [ -s /w/token ] && break; sleep 0.05; done
/usr/lib/wine/wine64 /w/smoke.exe 7455 "$(cat /w/token)" "sqlite:///w/smoke.sqlite?create=1" 2>/dev/null
'
