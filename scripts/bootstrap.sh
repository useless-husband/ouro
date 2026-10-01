#!/bin/sh
# Three-stage bootstrap.
#
#   stage 1: ouro built by the system C compiler (build/ouro)
#   stage 2: ouro built by stage 1
#   stage 3: ouro built by stage 2
#
# Stages 2 and 3 are the same program compiled by the same compiler
# source, so they must be byte-identical.  The assembly that stage 1 and
# stage 2 produce for every source file must also be identical: that
# checks the compiler compiled itself correctly, not just consistently.
set -eu
OPT=${OPT:--O1}
S1=build/ouro
[ -x "$S1" ] || { echo "build stage 1 first (make)"; exit 1; }

now() { python3 -c 'import time; print(time.time())'; }

build_stage() { # compiler outdir
  rm -rf "$2"
  mkdir -p "$2"
  for f in src/*.c; do
    "$1" $OPT -c -o "$2/$(basename "$f" .c).o" "$f"
  done
  "$1" -o "$2/ouro" "$2"/*.o
}

t0=$(now)
build_stage "$S1" build/stage2
t1=$(now)
build_stage build/stage2/ouro build/stage3
t2=$(now)

status=0
if cmp -s build/stage2/ouro build/stage3/ouro; then
  echo "stage2 and stage3 binaries are identical"
else
  echo "MISMATCH: stage2 and stage3 binaries differ"
  status=1
fi
for f in src/*.c; do
  b=$(basename "$f" .c)
  "$S1" $OPT -S -o "build/stage2/$b.s1.s" "$f"
  build/stage2/ouro $OPT -S -o "build/stage2/$b.s2.s" "$f"
  if ! cmp -s "build/stage2/$b.s1.s" "build/stage2/$b.s2.s"; then
    echo "MISMATCH: stage1 and stage2 emit different assembly for $f"
    status=1
  fi
done
[ $status -eq 0 ] && echo "stage1 and stage2 emit identical assembly for all $(ls src/*.c | wc -l | tr -d ' ') source files"
python3 -c "print('stage2 build: %.2f s, stage3 build: %.2f s' % ($t1 - $t0, $t2 - $t1))"
shasum -a 256 build/stage2/ouro build/stage3/ouro
exit $status
