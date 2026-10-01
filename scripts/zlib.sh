#!/bin/sh
# Builds zlib 1.3.1 from its public source with ouro and checks it:
#   * zlib's own test/example.c self-test passes;
#   * minigzip built by ouro round-trips data with the system gzip in
#     both directions;
#   * its compressed output is byte-identical to that of a minigzip
#     built by clang from the same source.
# The source is fetched into build/zlib at test time.
#
# usage: scripts/zlib.sh [ouro-binary] [-O0|-O1]
set -u
OURO=${1:-build/ouro}
OPT=${2:--O1}
OURO=$(cd "$(dirname "$OURO")" && pwd)/$(basename "$OURO")
VER=1.3.1
SHA=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
mkdir -p build/zlib
cd build/zlib || exit 1
f=zlib-$VER.tar.gz
[ -f $f ] || curl -fsSLO https://github.com/madler/zlib/releases/download/v$VER/$f || exit 1
echo "$SHA  $f" | shasum -a 256 -c - >/dev/null || { echo "checksum mismatch for $f"; exit 1; }
rm -rf zlib-$VER obj
tar xzf $f
mkdir -p obj
LIB="adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback inffast inflate inftrees trees uncompr zutil"
for m in $LIB; do
  "$OURO" $OPT -w -DZ_HAVE_UNISTD_H -Izlib-$VER -c -o obj/$m.o zlib-$VER/$m.c || exit 1
done
"$OURO" $OPT -w -DZ_HAVE_UNISTD_H -Izlib-$VER -o example zlib-$VER/test/example.c obj/*.o || exit 1
"$OURO" $OPT -w -DZ_HAVE_UNISTD_H -Izlib-$VER -o minigzip zlib-$VER/test/minigzip.c obj/*.o || exit 1
srcs=""
for m in $LIB; do srcs="$srcs zlib-$VER/$m.c"; done
clang -O2 -w -DZ_HAVE_UNISTD_H -Izlib-$VER -o minigzip-clang zlib-$VER/test/minigzip.c $srcs || exit 1

status=0
if ./example >example.log 2>&1; then
  echo "example: zlib self-test passed"
else
  echo "example: FAILED"
  cat example.log
  status=1
fi

# Test inputs: compressible text, incompressible bytes from a fixed-seed
# generator, and an empty file.
cat ../../src/*.c >text.dat
python3 -c "import random,sys; r=random.Random(1); sys.stdout.buffer.write(bytes(r.getrandbits(8) for _ in range(1<<21)))" >random.dat
: >empty.dat
for d in text.dat random.dat empty.dat; do
  ./minigzip <$d >$d.gz &&
    gzip -dc $d.gz | cmp -s - $d &&
    gzip -c $d | ./minigzip -d | cmp -s - $d &&
    ./minigzip-clang <$d | cmp -s - $d.gz
  if [ $? -eq 0 ]; then
    echo "minigzip: $d ($(wc -c <$d | tr -d ' ') bytes) round-trips with gzip; output identical to clang's build"
  else
    echo "minigzip: FAILED on $d"
    status=1
  fi
done
exit $status
