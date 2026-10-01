#!/bin/sh
# Differential testing with random programs: tests/fuzz/gen.py writes a
# well-defined C program for each seed; clang's build is the reference
# and ouro's -O0 and -O1 builds must print exactly the same.  A failing
# seed is printed so it can be reproduced: tests/fuzz/gen.py SEED.
#
# usage: scripts/fuzz.sh [ouro-binary] [first-seed] [count]
set -u
OURO=${1:-build/ouro}
FIRST=${2:-1}
COUNT=${3:-100}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/ouro-fuzz.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
fail=0
seed=$FIRST
last=$((FIRST + COUNT - 1))
while [ $seed -le $last ]; do
  python3 tests/fuzz/gen.py $seed >"$TMP/p.c"
  clang -O0 -w -ffp-contract=off -o "$TMP/ref" "$TMP/p.c" || { echo "seed $seed: clang failed"; fail=$((fail + 1)); seed=$((seed + 1)); continue; }
  perl -e 'alarm 20; exec @ARGV' "$TMP/ref" >"$TMP/ref.out" 2>&1
  echo "exit $?" >>"$TMP/ref.out"
  for opt in -O0 -O1; do
    if ! OURO_VERIFY=1 "$OURO" -w $opt -o "$TMP/t" "$TMP/p.c" 2>"$TMP/err"; then
      echo "seed $seed $opt: ouro failed to compile"
      head -5 "$TMP/err"
      fail=$((fail + 1))
      continue
    fi
    perl -e 'alarm 20; exec @ARGV' "$TMP/t" >"$TMP/out" 2>&1
    echo "exit $?" >>"$TMP/out"
    if ! cmp -s "$TMP/ref.out" "$TMP/out"; then
      echo "seed $seed $opt: output differs from clang"
      diff "$TMP/ref.out" "$TMP/out" | head -6
      fail=$((fail + 1))
    fi
  done
  seed=$((seed + 1))
done
echo "fuzz: seeds $FIRST..$last, $fail failures"
[ $fail -eq 0 ]
