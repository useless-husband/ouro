#!/bin/sh
# Unit tests, preprocessor tests, diagnostic tests and optimizer (IR)
# tests.
#
# usage: scripts/check.sh [ouro-binary]
set -u
OURO=${1:-build/ouro}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/ouro-check.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
fail=0

# Unit tests: every module except the driver, linked with tests/unit.
${CC:-cc} -std=c11 -g -w -o "$TMP/unit" tests/unit/unit.c $(ls src/*.c | grep -v '/main\.c$') &&
  "$TMP/unit" || fail=$((fail + 1))

# Preprocessor: same token stream as clang -E.
n=0
for f in tests/pp/*.c; do
  "$OURO" -E -P "$f" >"$TMP/a" 2>&1
  clang -E -P "$f" >"$TMP/b" 2>/dev/null
  if python3 tests/pp/pp_compare.py "$TMP/a" "$TMP/b" >"$TMP/msg"; then
    n=$((n + 1))
  else
    echo "FAIL pp $f: $(cat "$TMP/msg")"
    fail=$((fail + 1))
  fi
done
echo "pp: $n files produce the same tokens as clang -E"

# Diagnostics: the first line of stderr must be the expected message at
# the expected position, followed by the source line and a caret.
n=0
for f in tests/diag/*.c; do
  want=$(sed -n 's|^// expect: ||p' "$f" | head -1)
  if "$OURO" -c -o "$TMP/o.o" "$f" 2>"$TMP/err"; then
    echo "FAIL diag $f: compiled without error"
    fail=$((fail + 1))
    continue
  fi
  got=$(grep -v '^In file included' "$TMP/err" | head -1)
  if [ "$got" != "$f:$want" ]; then
    echo "FAIL diag $f"
    echo "    want: $f:$want"
    echo "    got:  $got"
    fail=$((fail + 1))
  elif ! sed -n '3p' "$TMP/err" | grep -q '\^$'; then
    echo "FAIL diag $f: no caret line"
    fail=$((fail + 1))
  else
    n=$((n + 1))
  fi
done
echo "diag: $n diagnostics with the expected message, position and caret"

# Optimizer: CHECK patterns over --dump-ir output.
n=0
for f in tests/ir/*.c; do
  opts=$(sed -n 's|^// RUN: ||p' "$f" | head -1)
  if ! "$OURO" $opts --dump-ir -S -o "$TMP/s.s" "$f" 2>"$TMP/ir"; then
    echo "FAIL ir $f: compile error"
    fail=$((fail + 1))
  elif python3 tests/ir/filecheck.py "$f" "$TMP/ir"; then
    n=$((n + 1))
  else
    fail=$((fail + 1))
  fi
done
echo "ir: $n optimizer tests pass"

# Instruction selection and allocation: CHECK patterns over the assembly.
n=0
for f in tests/asm/*.c; do
  opts=$(sed -n 's|^// RUN: ||p' "$f" | head -1)
  if ! "$OURO" $opts -S -o "$TMP/s.s" "$f" 2>"$TMP/err"; then
    echo "FAIL asm $f: compile error"
    fail=$((fail + 1))
  elif python3 tests/ir/filecheck.py "$f" "$TMP/s.s"; then
    n=$((n + 1))
  else
    fail=$((fail + 1))
  fi
done
echo "asm: $n code generation tests pass"
[ $fail -eq 0 ]
