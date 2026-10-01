#!/bin/sh
# Builds SQLite 3.50.4 (the 263,000-line amalgamation and its shell) with
# ouro and runs tests/sqlite/workload.sql against a fresh database file;
# the output must be identical to that of a clang build.  The source is
# fetched into build/sqlite at test time and checked against its SHA-256.
#
# usage: scripts/sqlite.sh [ouro-binary] [-O0|-O1]
set -u
OURO=${1:-build/ouro}
OPT=${2:--O1}
OURO=$(cd "$(dirname "$OURO")" && pwd)/$(basename "$OURO")
WORK=$(pwd)/tests/sqlite/workload.sql
ZIP=sqlite-amalgamation-3500400.zip
SHA=$(cat tests/sqlite/sha256)
mkdir -p build/sqlite
cd build/sqlite || exit 1
[ -f $ZIP ] || curl -fsSLO https://www.sqlite.org/2025/$ZIP || exit 1
echo "$SHA  $ZIP" | shasum -a 256 -c - >/dev/null || { echo "checksum mismatch for $ZIP"; exit 1; }
rm -rf sqlite-amalgamation-3500400 && unzip -q $ZIP && cd sqlite-amalgamation-3500400 || exit 1
# Apple's malloc zones and statfs-based locking styles are not needed.
FLAGS="-DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_WITHOUT_ZONEMALLOC -DSQLITE_ENABLE_LOCKING_STYLE=0 -DSQLITE_ENABLE_FTS5"
t0=$(python3 -c 'import time; print(time.time())')
"$OURO" $OPT -w $FLAGS -c -o sqlite3.o sqlite3.c || exit 1
"$OURO" $OPT -w $FLAGS -c -o shell.o shell.c || exit 1
"$OURO" -o sqlite3-ouro sqlite3.o shell.o || exit 1
t1=$(python3 -c 'import time; print(time.time())')
clang -O1 -w $FLAGS -o sqlite3-clang sqlite3.c shell.c || exit 1
rm -f db-ouro db-clang
./sqlite3-ouro db-ouro <"$WORK" >out-ouro.txt 2>&1
./sqlite3-clang db-clang <"$WORK" >out-clang.txt 2>&1
python3 -c "print('built sqlite3 with ouro $OPT in %.1f s' % ($t1 - $t0))"
if cmp -s out-ouro.txt out-clang.txt && grep -qx ok out-ouro.txt; then
  echo "sqlite: workload output identical to the clang build ($(wc -l <out-ouro.txt | tr -d ' ') lines, integrity_check ok)"
else
  echo "sqlite: output differs from the clang build"
  diff out-clang.txt out-ouro.txt | head -20
  exit 1
fi
