#!/bin/sh
# Builds Lua 5.4.7 from its public source with ouro and runs Lua's own
# test suite (https://www.lua.org/tests/).  Sources are fetched into
# build/lua at test time and checked against known SHA-256 sums.
#
# Two builds are tested:
#   lua    the normal interpreter (-DLUA_USE_MACOSX), with the suite's C
#          modules (libs/*.so) also compiled by ouro as dynamic libraries
#   lua-T  the interpreter with Lua's internal test library (ltests.c),
#          which turns on internal assertions and the C API tests
#
# Both run `all.lua` with _port=true, which skips only the tests that
# depend on the platform (the interactive-prompt test in main.lua needs
# GNU readline and fails the same way with clang).
#
# usage: scripts/lua.sh [ouro-binary] [-O0|-O1]
set -u
OURO=${1:-build/ouro}
OPT=${2:--O1}
OURO=$(cd "$(dirname "$OURO")" && pwd)/$(basename "$OURO")
DIR=build/lua
VER=5.4.7
SRC_SHA=9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30
TEST_SHA=8a4898ffe4c7613c8009327a0722db7a41ef861d526c77c5b46114e59ebf811e

mkdir -p "$DIR"
cd "$DIR" || exit 1
fetch() { # url sha
  f=$(basename "$1")
  if [ ! -f "$f" ]; then
    curl -fsSLO "$1" || exit 1
  fi
  echo "$2  $f" | shasum -a 256 -c - >/dev/null || { echo "checksum mismatch for $f"; exit 1; }
}
fetch https://www.lua.org/ftp/lua-$VER.tar.gz $SRC_SHA
fetch https://www.lua.org/tests/lua-$VER-tests.tar.gz $TEST_SHA
rm -rf lua-$VER lua-$VER-tests obj obj-T
tar xzf lua-$VER.tar.gz
tar xzf lua-$VER-tests.tar.gz
mkdir -p obj obj-T

status=0
start=$(python3 -c 'import time; print(time.time())')
for f in lua-$VER/src/*.c; do
  b=$(basename "$f" .c)
  [ "$b" = luac ] && continue
  "$OURO" $OPT -w -DLUA_USE_MACOSX -c -o "obj/$b.o" "$f" || exit 1
done
"$OURO" -o lua obj/*.o || exit 1
end=$(python3 -c 'import time; print(time.time())')
python3 -c "print('built lua with ouro $OPT in %.2f s' % ($end - $start))"

for f in lua-$VER/src/*.c lua-$VER-tests/ltests/ltests.c; do
  b=$(basename "$f" .c)
  [ "$b" = luac ] && continue
  "$OURO" $OPT -w -DLUA_USE_MACOSX -Ilua-$VER/src -Ilua-$VER-tests/ltests \
    '-DLUA_USER_H="ltests.h"' -c -o "obj-T/$b.o" "$f" || exit 1
done
"$OURO" -o lua-T obj-T/*.o || exit 1

(cd lua-$VER-tests/libs &&
  for l in lib1 lib11 lib2 lib21; do
    "$OURO" $OPT -w -shared -undefined dynamic_lookup -I../../lua-$VER/src -o $l.so $l.c || exit 1
  done &&
  "$OURO" $OPT -w -shared -undefined dynamic_lookup -I../../lua-$VER/src -o lib2-v2.so lib22.c) ||
  exit 1

for exe in lua lua-T; do
  log=$exe-tests.log
  t0=$(python3 -c 'import time; print(time.time())')
  (cd lua-$VER-tests && perl -e 'alarm 1200; exec @ARGV' ../$exe -e"_port=true" all.lua) >"$log" 2>&1
  st=$?
  t1=$(python3 -c 'import time; print(time.time())')
  files=$(grep -c '^\*\*\*\*\* FILE' "$log")
  if [ $st -eq 0 ] && grep -q 'final OK' "$log"; then
    python3 -c "print('$exe: all.lua passed (%s test files, final OK) in %.1f s' % ($files, $t1 - $t0))"
  else
    echo "$exe: all.lua FAILED (exit $st) after $files test files; see $DIR/$log"
    tail -5 "$log"
    status=1
  fi
done
exit $status
