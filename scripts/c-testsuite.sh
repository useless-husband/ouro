#!/bin/sh
# Runs the public c-testsuite (https://github.com/c-testsuite/c-testsuite)
# against ouro.  The suite is fetched into build/ at test time, not
# committed.  A test passes when it compiles, exits with status 0 and
# prints exactly the expected output.
#
# usage: scripts/c-testsuite.sh [ouro-binary] [-O0|-O1]
set -u
OURO=${1:-build/ouro}
OPT=${2:--O1}
DIR=build/c-testsuite
REV=${C_TESTSUITE_REV:-master}
if [ ! -d "$DIR/tests/single-exec" ]; then
  rm -rf "$DIR"
  git clone -q https://github.com/c-testsuite/c-testsuite "$DIR" || exit 1
  (cd "$DIR" && git checkout -q "$REV")
fi
OURO=$(cd "$(dirname "$OURO")" && pwd)/$(basename "$OURO")
TMP=$(mktemp -d "${TMPDIR:-/tmp}/ouro-cts.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
pass=0
fail=0
: >build/c-testsuite.failures
for t in "$DIR"/tests/single-exec/*.c; do
  name=$(basename "$t" .c)
  if ! "$OURO" -w $OPT -o "$TMP/a.out" "$t" >"$TMP/cc.log" 2>&1; then
    echo "$name compile: $(grep -m1 -E 'error' "$TMP/cc.log" | cut -c1-160)" >>build/c-testsuite.failures
    fail=$((fail + 1))
    continue
  fi
  (cd "$TMP" && perl -e 'alarm 10; exec @ARGV' ./a.out >"$TMP/out" 2>/dev/null)
  st=$?
  if [ $st -ne 0 ]; then
    echo "$name exit status $st" >>build/c-testsuite.failures
    fail=$((fail + 1))
  elif ! cmp -s "$TMP/out" "$t.expected"; then
    echo "$name wrong output" >>build/c-testsuite.failures
    fail=$((fail + 1))
  else
    pass=$((pass + 1))
  fi
done
total=$((pass + fail))
echo "c-testsuite ($OPT): $pass/$total passed (commit $(cd "$DIR" && git rev-parse --short HEAD))"
[ $fail -eq 0 ] || echo "failures listed in build/c-testsuite.failures"
