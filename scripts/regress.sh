#!/bin/sh
# Differential regression tests: every tests/regress/*.c program is
# compiled with clang (the reference) and with ouro at -O0 and -O1; the
# outputs and exit codes must match.  The IR verifier is on.
#
# usage: scripts/regress.sh [ouro-binary] [test.c ...]
set -u
OURO=${1:-build/ouro}
[ $# -gt 0 ] && shift
TESTS=${*:-tests/regress/*.c}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/ouro-regress.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
pass=0
fail=0
for t in $TESTS; do
  name=$(basename "$t" .c)
  if ! clang -w -O0 -o "$TMP/ref" "$t" 2>"$TMP/ref.err"; then
    echo "SKIP $name (clang cannot compile it)"
    continue
  fi
  "$TMP/ref" >"$TMP/ref.out" 2>&1
  echo "exit $?" >>"$TMP/ref.out"
  for opt in -O0 -O1; do
    if ! OURO_VERIFY=1 "$OURO" -w $opt -o "$TMP/t" "$t" 2>"$TMP/err"; then
      echo "FAIL $name $opt: compile error"
      sed 's/^/    /' "$TMP/err" | head -20
      fail=$((fail + 1))
      continue
    fi
    "$TMP/t" >"$TMP/out" 2>&1
    echo "exit $?" >>"$TMP/out"
    if cmp -s "$TMP/ref.out" "$TMP/out"; then
      pass=$((pass + 1))
    else
      echo "FAIL $name $opt: output differs from clang"
      diff "$TMP/ref.out" "$TMP/out" | head -20 | sed 's/^/    /'
      fail=$((fail + 1))
    fi
  done
done
echo "regress: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
