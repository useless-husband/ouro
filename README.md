# ouro

**A self-hosting C11 compiler for arm64 macOS, written in C.**

ouro compiles its own source, and the compiler it produces compiles the source
again to a byte-identical binary. It has its own preprocessor, parser and type
checker, lowers to an SSA intermediate representation, optimizes it (mem2reg,
sparse conditional constant propagation, CSE, loop-invariant code motion,
redundant load elimination, DCE, CFG cleanup), selects arm64 instructions,
allocates registers with linear scan, and emits assembly that follows Apple's
arm64 calling convention. It drives the system assembler and linker itself.

[繁體中文說明](README.zh-TW.md) · [Design](docs/DESIGN.md) · [導讀（中文）](docs/導讀.zh-TW.md)

```
$ make && make bootstrap
stage2 and stage3 binaries are identical
stage1 and stage2 emit identical assembly for all 20 source files
stage2 build: 0.74 s, stage3 build: 0.79 s
b792e17916f40a02169d4d0428e500d81eaa69c0901848981abb2e0aaf8e76f9  build/stage2/ouro
b792e17916f40a02169d4d0428e500d81eaa69c0901848981abb2e0aaf8e76f9  build/stage3/ouro
```

Stage 1 is ouro built by clang, stage 2 is ouro built by stage 1, stage 3 is ouro
built by stage 2. The self-compiled stage 2 also passes the regression tests and
the c-testsuite (`make stage2-test`, run in CI) and builds Lua, zlib and SQLite
correctly. Every claim below comes with the command that checks it.

## What it can compile

| Check | Result | Command |
|---|---|---|
| [c-testsuite](https://github.com/c-testsuite/c-testsuite) (all 220 single-exec tests, fetched at test time, commit 5c72756) | **220/220** at -O0 and at -O1 | `make c-testsuite` |
| [Lua 5.4.7](https://www.lua.org) built from its release tarball, running Lua's own test suite `all.lua` with `_port=true` (which skips only platform-dependent tests; the interactive-prompt test needs GNU readline and fails the same way with clang) | all 27 test files pass ("final OK"), also with Lua's internal assertion and C-API test library compiled in; the suite's C modules are built by ouro as dynamic libraries | `make lua` |
| [zlib 1.3.1](https://zlib.net) | its `example` self-test passes; `minigzip` round-trips with the system `gzip` both ways and writes the same bytes as a clang build | `make zlib` |
| [SQLite 3.50.4](https://sqlite.org) (the 263,000-line amalgamation and its shell, FTS5 enabled) | compiles at -O0 and -O1; a SQL workload (recursive CTEs, window functions, JSON, FTS5, transactions, `VACUUM`, `integrity_check`) prints exactly what a clang build prints | `make sqlite` |
| Calling convention | callers and callees generated for 80 signatures per seed, compiled by ouro and clang in every combination, print the same as an all-clang build | `make abi` |
| Random programs | well-defined programs from a seeded generator print the same as with clang at -O0 and -O1: 1,000 seeds (2000–2999) on the final compiler, 0 differences (`make test` runs 40) | `make fuzz` |
| Regression programs | output identical to clang's at -O0 and -O1 | `make test` |

`make test` also runs unit tests, preprocessor tests (token streams identical to
`clang -E`), diagnostic tests (exact message, position and caret), and FileCheck
style tests of the optimizer's IR and the generated assembly.

## A look inside

```c
int sum(const int *a, int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s += a[i];
  return s;
}
```

`ouro -O1 --dump-ir -S sum.c` prints the optimized IR (the locals are now SSA
values and phis) and writes the assembly:

```
function _sum {                          _sum:
bb0:                                     LBB0_0:
  %0 = param i64 x0                          mov w9, #0
  %1 = param i32 x1                          mov w10, #0
  jmp bb2                                    b LBB0_2
bb1:  ; preds bb2                        LBB0_1:
  %3 = sext32 i64 %10                        ldr w11, [x0, w9, sxtw #2]
  %4 = shl i64 %3, 2                         add w10, w10, w11
  %5 = add i64 %0, %4                        add w9, w9, #1
  %6 = load i32 %5                       LBB0_2:
  %7 = add i32 %11, %6                       cmp w9, w1
  %8 = add i32 %10, 1                        b.lt LBB0_1
  jmp bb2                                LBB0_3:
bb2:  ; preds bb0 bb1                        mov x0, x10
  %10 = phi i32 [0, bb0], [%8, bb1]          ret
  %11 = phi i32 [0, bb0], [%7, bb1]
  %12 = slt i32 %10, %1
  br %12, bb1, bb3
bb3:  ; preds bb2
  ret %11
}
```

The index computation folds into a register-offset load, the comparison into the
branch, and the phi copies at the back edge disappear because the allocator gives
each value the register of the phi it feeds. Errors point at the source,
including through macros:

```
$ ouro -c oops.c
oops.c:3:10: error: use of undeclared identifier 'PI'
    3 |   return AREA(r);
      |          ^
oops.c:1:18: note: expanded from macro 'AREA'
    1 | #define AREA(r) (PI * (r) * (r))
      |                  ^
```

## Performance

Measured with `scripts/bench.py --runs 7` on an Apple M5 (10 cores) running macOS
27.0, Apple clang 17.0.0. The machine was shared with other jobs while measuring
(load average about 8), so expect a few percent of noise; each figure is the
median of 7 runs, in seconds. Ratios above 1 mean ouro's code is faster. All
builds of a program must print the same output for its numbers to count.

| program (run time, s) | clang -O0 | clang -O2 | ouro -O0 | ouro -O1 | ouro -O1 vs clang -O0 | ouro -O1 vs clang -O2 |
|---|---:|---:|---:|---:|---:|---:|
| binarytrees.c | 0.757 | 0.678 | 0.747 | 0.710 | 1.07x | 0.96x |
| fannkuch.c | 0.210 | 0.162 | 0.276 | 0.129 | 1.62x | 1.25x |
| matmul.c | 0.259 | 0.029 | 0.431 | 0.058 | 4.47x | 0.51x |
| nbody.c | 0.920 | 0.282 | 1.043 | 0.348 | 2.64x | 0.81x |
| sieve.c | 1.261 | 1.133 | 2.273 | 1.069 | 1.18x | 1.06x |
| Lua 5.4.7 running `tests/bench/fib.lua` | 0.235 | 0.079 | 0.195 | 0.142 | 1.65x | 0.56x |
| SQLite 3.50.4 running `tests/bench/sqlite-bench.sql` | 0.659 | 0.271 | 0.641 | 0.390 | 1.69x | 0.69x |

ouro -O1 beats clang -O0 on every program, including the Lua interpreter
and SQLite, and ranges from about half of clang -O2's speed (where clang
inlines, vectorizes and unrolls: matmul, the Lua interpreter) through 0.7x on
SQLite to parity on allocation- and memory-bound code. It does no inlining,
vectorization, unrolling or tail-call elimination.

Compile time, sequentially compiling every file to assembly (median of 3 runs;
each file is a separate process, so on many small files much of the gap is
clang's start-up time, and on SQLite it is clang's optimizer):

| compile to assembly (s) | clang -O0 | clang -O2 | ouro -O0 | ouro -O1 | ouro -O1 vs clang -O0 | ouro -O1 vs clang -O2 |
|---|---:|---:|---:|---:|---:|---:|
| Lua 5.4.7 (33 files, 23,940 lines) | 1.186 | 2.090 | 0.263 | 0.290 | 4.09x | 7.20x |
| SQLite 3.50.4 amalgamation (1 file, 262,899 lines) | 0.723 | 6.327 | 0.280 | 0.388 | 1.86x | 16.32x |
| ouro itself (20 files, 15,597 lines) | 0.769 | 1.686 | 0.111 | 0.134 | 5.75x | 12.60x |

`--time` reports the time spent per phase. Building an executable additionally
runs the system assembler, which takes about 0.05 s per file and dominates build
times.

## How it works

```
source ─► lexer ─► preprocessor ─► parser/type checker ─► IR generation ─► optimizer ─► instruction selection ─► register allocation ─► assembly ─► as ─► ld
```

- **Preprocessor**: a pull-based token stream over a stack of sources (files and
  macro expansions), with Prosser's hidesets for recursion. Its output is compared
  token by token with `clang -E`.
- **Front end**: one pass of parsing and type checking. Every implicit conversion
  becomes an explicit cast in the tree, and constant expressions are folded as the
  tree is built, never folding something the standard leaves undefined.
- **IR**: SSA over a control-flow graph. Constants are rematerialized at each use.
  Values narrower than 32 bits are kept sign- or zero-extended in 32-bit
  registers, which is what Apple's ABI expects at calls.
- **Optimizer**: mem2reg (Cytron et al.), SCCP (Wegman and Zadeck) that folds
  exactly as arm64 computes, combine, dominator-tree CSE, block-local redundant
  load elimination with conservative aliasing, LICM, DCE. A verifier checks the IR
  after every pass in the tests.
- **Back end**: pattern-based instruction selection (compare-and-branch,
  immediates, shifted operands, register-offset addressing, jump tables), linear
  scan register allocation on live ranges with holes, spill choice weighted by
  loop depth, phi coalescing hints, parallel-move resolution.
- **ABI**: Apple's arm64 rules: packed stack arguments, variadic arguments always
  on the stack, caller-extended narrow arguments, signed `char`, 64-bit `long
  double`, x18 untouched, HFAs in SIMD registers.

[docs/DESIGN.md](docs/DESIGN.md) explains each stage, the hard problems and the
choices that were rejected.

### Why ouro ships its own libc headers

Apple's SDK headers use Clang blocks, availability and nullability attributes,
symbol aliases and inline functions built on compiler builtins. Rather than
implement those extensions, ouro ships 41 small headers in `include/` that
declare what C programs need, with Darwin's layouts and constants where the binary
interface depends on them (`__stdinp`, `errno` as `*__error()`, the 48-int
`jmp_buf`, the 144-byte `struct stat`, `realpath`'s `$DARWIN_EXTSN` symbol). A
program that needs a header or declaration ouro does not provide will not compile
until one is added.

## Using it

```sh
make                         # build/ouro
build/ouro -o hello hello.c  # compile and link
build/ouro -S -O1 f.c        # assembly
build/ouro -E f.c            # preprocess
build/ouro --help
```

Options: `-o`, `-c`, `-S`, `-E`, `-O0`/`-O1` (default `-O1`; `-O2` and up mean
`-O1`), `-I`, `-D`, `-U`, `-w`, `-Werror`, `-l`, `-L`, `-Wl,`, `-shared`,
`--dump-ir`, `--dump-ir-raw`, `--dump-mir`, `--time`. Common flags of other
compilers (`-g`, `-std=`, `-W...`, `-f...`) are accepted and ignored.
`MACOSX_DEPLOYMENT_TARGET` is honoured when linking.

## Limitations

- arm64 macOS only. ouro needs Xcode's command line tools for `as` and `ld`.
- Not supported: `_Complex`, `_Thread_local`, real `_Atomic` operations (the
  qualifier is accepted and ignored), multi-dimensional variable length arrays and
  pointer arithmetic on pointers to them, inline assembly, computed `goto`, the
  GNU `a ?: b` form. `long double` is `double`, as on Apple's platforms.
- Diagnostics stop at the first error; there is no error recovery.
- The optimizer has no inlining, unrolling, vectorization, tail-call elimination
  or type-based alias analysis.
- Only the headers in `include/` are available (see above).
- The tested programs are the c-testsuite, Lua, zlib, SQLite, ouro itself, and
  the generated and hand-written tests in this repository; other code bases may
  well find missing features or headers.
- Memory is never freed during a compilation: SQLite's amalgamation needs 550 MB.

## Related work

- [chibicc](https://github.com/rui314/chibicc) by Rui Ueyama is a small,
  self-hosting C11 compiler for x86-64 Linux that compiles real programs; it
  generates code straight from the syntax tree with a stack discipline. ouro
  shares its goal of a readable compiler that can build real software, but has an
  SSA optimizer and a register allocator and targets Apple's arm64 ABI.
- [cproc](https://github.com/michaelforney/cproc) with
  [QBE](https://c9x.me/compile/) is the closest design: a C11 front end feeding a
  separate SSA back end with register allocation. cproc is tested on Linux and
  FreeBSD (x86_64, aarch64, riscv64) and needs an external preprocessor; QBE lists
  amd64 (Linux and macOS), arm64 and riscv64. ouro is one code base that owns
  every stage, including the preprocessor and headers, and targets Apple's arm64
  convention specifically.
- [TinyCC](https://bellard.org/tcc/) compiles very fast in a single pass and
  supports several targets, with little optimization.
- [lcc](https://github.com/drh/lcc) is the classic retargetable C compiler,
  described in a book; it generates code from trees without an SSA form.

To my knowledge, few compilers combine all of these in one self-contained,
self-hosting code base for arm64 macOS: its own preprocessor and headers, an SSA
optimizer, and a register allocator following Apple's calling convention. ouro
is meant as a readable example of that, not as a replacement for clang.

## Development

```sh
make test         # unit, preprocessor, diagnostic, IR, assembly, regression, ABI and random-program tests
make bootstrap    # three-stage self-compilation check
make stage2-test  # the self-compiled compiler runs the regression, unit/IR and c-testsuite tests
make c-testsuite  # fetches the suite into build/
make lua          # fetches Lua 5.4.7 and its tests into build/ (checksums verified)
make zlib         # fetches zlib 1.3.1 into build/ (checksum verified)
make sqlite       # fetches SQLite 3.50.4 into build/ (checksum verified)
make fuzz         # 500 random programs; FUZZ_SEEDS=N for more
make abi          # calling-convention tests, 10 seeds
make lint         # clang -Wall -Wextra -Wpedantic -Wshadow -Werror
make sanitize     # ouro built with UBSan runs the regression and ABI tests
python3 scripts/bench.py   # the performance tables above
```

`OURO_VERIFY=1` turns on the IR verifier after every pass (the test scripts set
it). When ouro and clang disagree on a program, `tests/fuzz/reduce.py` shrinks it
while checking with UBSan that it stays well defined.

## License

[MIT](LICENSE)
