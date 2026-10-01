# ouro design

This document describes how ouro is put together, the problems that were hard,
and the choices that were made instead of the obvious alternatives. Line counts
refer to the sources at the time of writing (about 16,800 lines of C in `src/`).

## Pipeline

```
 source ──► lex.c ──► pp.c ──► parse.c/expr.c/init.c ──► irgen.c ──► optimizer ──► isel.c ──► regalloc.c ──► emit.c ──► as ──► ld
          tokens   expanded     typed AST, static       SSA IR      SSA IR       machine IR   physical      assembly
                   tokens       data images                                     (virtual      registers
                                                                                 registers)
```

`main.c` is the driver: it runs the pipeline for each C file (in a child process
when there are several, because the front end keeps global state), then runs the
system assembler (`as -arch arm64`) and linker (`ld` with `-platform_version` and
`-syslibroot` from `xcrun`) directly.

Memory comes from an arena and is never freed. A compiler is a batch process, and
for ordinary files this costs little (Lua's `lvm.c` peaks at 69 MB resident), but
SQLite's 263,000-line amalgamation needs 550 MB: tokens produced by macro
expansion are copies, and every function's IR and machine code stay allocated.
Releasing per-function memory after its code is emitted would fix most of it; it
was not done because a few objects created during code generation (cached pointer
types, helper symbols) outlive the function.

## Front end

### Lexer (`lex.c`)

Translation phases 1–2 (line endings, backslash-newline splices) run over the
whole file first. Removed newlines are re-inserted at the end of the logical line,
so every following line keeps its physical number. Tokens carry an integer kind:
single-character punctuators are their own character code (`tok->kind == '{'`),
multi-character ones and keywords have enum values. Numbers stay pp-numbers and
literals keep their spelling until preprocessing is over, because `#` and `##`
operate on spellings.

Identifiers are interned. Each `Ident` also holds the identifier's current macro
definition and its innermost declarations (ordinary and tag namespace), so macro
lookup and name lookup are a pointer dereference, not a hash probe per scope.

### Preprocessor (`pp.c`)

The preprocessor is a pull-based token stream over a stack of *sources*: files
(whose tokens may contain directives) and macro expansions (whose tokens never
do). Asking for the next token pops exhausted sources, runs directives, and
expands macros; an expansion is pushed as a new source, so it is rescanned
together with the rest of the input. That is exactly what the standard requires
for a function-like macro name at the end of an expansion, which takes its
arguments from the text that follows.

Recursion is controlled with Prosser's hidesets: each token carries the set of
macros it came out of and is never expanded by one of them; the hideset of a
function-like expansion is the intersection of the name's and the closing
parenthesis's hidesets plus the macro. Arguments are pre-expanded in isolation by
pushing them as a *barrier* source that `next` never reads past.

Other pieces: an `#if` evaluator of its own with C's unsigned/signed rules and
short-circuiting (`1 || 1/0` is fine), `defined`, `__has_include`,
`__has_attribute`/`__has_builtin` (answering for ouro's builtins), include guard
detection (a guarded header is not reopened), `#pragma once`,
`#pragma push_macro`/`pop_macro`, `__VA_OPT__`, GNU `, ## __VA_ARGS__`, `#line`
and GNU line markers, and `-E` output that inserts a space wherever two adjacent
tokens would otherwise lex as one.

The preprocessor is tested by comparing its token stream with `clang -E` on the C
standard's own examples and on a directive torture file (`tests/pp`).

### Parser and type checker (`parse.c`, `expr.c`, `init.c`, `type.c`)

Parsing and semantic analysis happen in one pass. Every implicit conversion
becomes an explicit `E_CAST` node, arrays and functions decay through an explicit
`E_DECAY`, and pointer arithmetic is a separate node with its scale, so the code
generator never applies C's conversion rules itself. Operations on constants are
folded while the tree is built, which makes integer constant expressions (array
sizes, case labels, enumerators, bit-field widths, static initializers) plain
constants by the time anyone looks at them. Folding never evaluates something the
standard leaves undefined (signed division overflow, out-of-range shifts or
float-to-int conversions); those are left for run time.

Declarators are parsed with a re-parse trick instead of placeholder types: in
`int (*fp)[3]` the nested declarator is skipped, the suffixes after it are parsed
into a type, then the nested part is parsed again with that type as its base.

Initializers (`init.c`) are parsed into a tree mirroring the object. The hard
parts are brace elision (an aggregate member without braces takes as many values
as it needs from the enclosing list) and designators, after which initialization
continues with the next member of the innermost designated aggregate
(`{ .s.a = 1, 2 }` sets `s.b` to 2). A value that is parsed before its target is
known (`struct S arr[] = { s1, 1, 2 }`: is `s1` a whole element or the first
scalar?) is held as a pending expression and consumed by whichever subobject it
fits. Static objects become a byte image plus relocations; automatic objects are
initialized by code, zeroing first only when the initializer does not cover every
byte.

Struct layout follows the AAPCS64 rules that Apple uses for bit-fields: a field is
placed in the next bits if it fits in a naturally aligned unit of its declared
type, otherwise it starts a new unit; zero-width fields round up to their type's
alignment; unnamed fields do not raise the struct's alignment.

### Diagnostics

Errors have the form `file:line:col: error: message`, followed by the source line
and a caret. When the offending token came from a macro expansion, the message
points at the macro use and a note shows the token inside the definition, with
its own caret. ouro stops at the first error; it does not attempt recovery.
`tests/diag` pins the exact message, position and caret for a set of common
errors.

## ouro's own C library headers

ouro does not parse Apple's SDK headers. They use Clang blocks (`^`), availability
and nullability attributes, `__asm` symbol aliases, inline functions written with
compiler builtins, and many `__has_feature` checks; supporting all of that would
be a large project unrelated to compiling C. Instead `include/` holds 41
small headers that declare what C programs (and ouro itself, Lua, zlib and
SQLite) need, with the layouts and constants of Darwin's libc where they matter for the
binary interface:

- `stdin`/`stdout`/`stderr` are Darwin's `__stdinp`/`__stdoutp`/`__stderrp`;
  `errno` is `*__error()`; `jmp_buf` is 48 ints; `struct stat` is the 144-byte
  64-bit-inode layout; `struct lconv`, `struct tm`, `struct sigaction` and the
  `<sys/wait.h>` macros follow Darwin.
- `int64_t` is `long long` and `intptr_t` is `long`, as in Darwin's headers, so
  format strings such as `PRId64` agree with libc.
- Darwin's ctype functions are real exported functions, so `<ctype.h>` just
  declares them; `realpath` needs the `__asm__("_realpath$DARWIN_EXTSN")` label
  that clang also uses; the classification macros of `<math.h>` are written in
  plain C.

The cost is coverage: a program that includes a header ouro does not ship (or
uses a declaration missing from one) fails to compile, and a layout mistake would
not be caught by the compiler. The ABI tests and the Lua/zlib builds exercise the
headers against the real libc.

## The intermediate representation (`ir.c`)

A conventional SSA form over a control-flow graph. Every instruction that produces
a value *is* that value; operands point to defining instructions. Constants and
symbol addresses are *floating* instructions that belong to no block; the
instruction selector rematerializes them at each use, which keeps their live
ranges to one instruction and lets them disappear into immediate operands.

Value types are `I32`, `I64`, `F32` and `F64`. Integers narrower than 32 bits never
appear as values: a `char` is carried in an `I32` that is already sign- or
zero-extended for its type (its *canonical form*). Narrow widths exist only as
memory types of loads and stores. This matches the Apple ABI, where callers extend
narrow arguments and callees extend narrow return values, and it means only
conversions between narrow types need an instruction.

Instructions are replaced lazily: `ir_replace(old, new)` records a forwarding
pointer and `ir_resolve_all` redirects operands afterwards. With `OURO_VERIFY=1`
(always set by the tests) a verifier runs after every pass: blocks end in exactly
one terminator, phis come first and match the predecessor list, no operand is a
replaced instruction or missing from every block, and within a block every operand
is defined before its use.

## The Apple arm64 calling convention (`abi.c`, `irgen.c`)

Apple's convention is AAPCS64 with documented differences, all of which ouro
implements and which `tests/abi` checks against clang in every caller/callee
combination:

| Rule | AAPCS64 | Apple, and ouro |
|---|---|---|
| Stack arguments | each in an 8-byte slot | packed at natural size and alignment (a `char` takes 1 byte) |
| Variadic arguments | in registers while they last | always on the stack, 8-byte slots |
| Narrow integer arguments | callee extends | caller extends; callee extends narrow return values |
| `char` | unsigned | signed |
| `long double` | 128-bit | same as `double` |
| x18 | platform register, usable on some systems | reserved, never touched |
| Frame record | | x29 always points at a valid frame record when a frame exists |

Classification: integers and pointers go in general registers, floats and doubles
in SIMD registers; homogeneous floating-point aggregates (one to four members of
one floating type, counted through nested structs and arrays) go in consecutive
SIMD registers, also when 32 bytes long; other aggregates up to 16 bytes in one
or two general registers; larger ones by reference to a caller-made copy, with
large results returned through a buffer whose address the caller passes in x8.
An aggregate is never split between registers and the stack, and once one does
not fit, the remaining registers of that class are not used for later arguments.
On the stack an HFA keeps its exact size and element alignment (a 12-byte
`{float x,y,z}` is followed directly by a `float`), while other small aggregates
are rounded up to whole 8-byte words. These facts were checked by compiling probes
with clang before the code was written, and the ABI test fails if one of them is
changed (we tried: switching to AAPCS64 stack slots makes it report mismatches).

`irgen.c` splits each argument into scalar *pieces* with explicit locations
(register number or stack offset and store width), so the back end never sees C
types. `va_list` is a plain pointer into the caller's stack argument area;
`va_start` is the address after the named stack arguments.

## Optimizer

`-O1` runs: CFG simplification, mem2reg, then twice SCCP, CFG simplification,
combine, CSE, redundant-load elimination, DCE, CFG simplification; then
loop-invariant code motion followed by another combine/CSE/DCE; finally critical
edges are split for the instruction selector. `-O0` only simplifies the CFG and
splits critical edges.

- **mem2reg** (`mem2reg.c`): Cytron et al. A stack slot is promoted when its
  address is used only as the address of non-volatile loads and stores of one
  memory type. Phis go at the iterated dominance frontier of the stores (the
  dominator tree and frontiers come from Cooper, Harvey and Kennedy's algorithm in
  `cfg.c`); renaming walks the dominator tree with an explicit stack. A narrow load
  is replaced by an explicit extension of the reaching value with the load's
  signedness, which keeps the canonical form even when one variable is read as
  both `char` and `unsigned char`; combine removes the extensions that are
  redundant. A function that calls `setjmp` is not promoted at all: its variables
  must survive `longjmp`, which only restores callee-saved registers.
- **SCCP** (`sccp.c`): Wegman and Zadeck. Values move down TOP → constant →
  BOTTOM, and blocks are only evaluated once an edge into them is executable, so a
  constant branch keeps the dead side out of the phis it feeds. Folding follows the
  target exactly: 32-bit operations wrap at 32 bits, shift amounts are taken modulo
  the width as arm64 does, and undefined operations (division by zero, `INT_MIN /
  -1`, out-of-range float-to-int conversions) are left alone. A unit test compares
  the folder with C arithmetic on 100,000 random operand pairs.
- **combine** (`opt.c`): local rewrites such as `x*2^k → x<<k`, unsigned division
  and remainder by powers of two, `(x+c1)+c2 → x+(c1+c2)`, redundant extensions,
  `cmp != 0 → cmp`, `cmp == 0 →` the inverse comparison.
- **CSE**: a preorder walk of the dominator tree with a scoped hash table of pure
  instructions (entries are removed in reverse order on the way back up, which
  keeps linear probing consistent).
- **Redundant loads** (`licm.c`): within a block, a load from a location with a
  known value (from an earlier load or store) is replaced by it. A store forgets
  every location it may overlap; two locations are known distinct only if they
  share a base value and their byte ranges are disjoint, or if their bases are
  different named objects (two globals, two stack slots, one of each). Calls and
  block copies forget everything. There is no type-based alias analysis.
- **LICM**: pure instructions whose operands are defined outside a natural loop
  move into a preheader, inner loops first. Speculating them is safe because no
  pure instruction can trap on arm64 (an integer division by zero yields 0).
- **DCE**: mark from instructions with side effects, sweep the rest; also at the
  machine level after instruction selection (`isel.c`), where folding a value into
  addressing modes can leave its computation unused.

## Instruction selection (`isel.c`)

Each IR value gets a virtual register; floating values are rematerialized next to
each use. Patterns matched across instructions:

- a comparison used only by the branch that ends its block becomes
  `cmp`/`fcmp` + `b.cond`, and a comparison with zero becomes `cbz`/`cbnz`
  (floating-point conditions use `mi`/`ls` so that NaN compares false, and
  inverting a condition code is always the exact logical negation);
- base + constant addresses fold into load/store offsets; base +
  (index << log2 size), with a 32-bit index sign- or zero-extended, becomes the
  register-offset form `[x0, w9, sxtw #2]`;
- `x + (y << k)` uses the shifted-register form; additions, comparisons and
  logical operations take immediate operands when the constant is encodable (the
  bitmask immediate check is property-tested against every encoding the Arm ARM's
  `DecodeBitMasks` defines);
- dense switches become jump tables (`adr`/`ldrsw`/`br` with a `.data_region
  jt32`), others a chain of compares.

Phis become parallel moves at the end of each predecessor (critical edges were
split, so such a predecessor has one successor). Calls become stores of stack
arguments, one parallel move into the argument registers (the callee address of
an indirect call goes to x17 in the same move, so no argument can overwrite it),
`bl`/`blr`, and a move or stores of the results.

Two invariants are checked here rather than trusted: no virtual register may be
live into the entry block (a use emitted before its definition; this caught two
real bugs), and a fold must never leave a value that some instruction still
needs uncomputed.

## Register allocation (`regalloc.c`)

Linear scan over live intervals with holes, after Wimmer and Mössenböck (2005),
without interval splitting.

1. Liveness: backward data flow over blocks on bit sets of virtual registers.
2. Intervals: each virtual register gets the sorted list of position ranges where
   it is live, built by walking blocks and instructions backwards. Positions are
   two per instruction (uses at 2i, definitions at 2i+1), so a value that dies at
   an instruction can share a register with the one it defines.
3. Allocation: intervals in order of start; an *active* set (covering the current
   position) and an *inactive* set (started, but in a hole). A register can be
   given to an interval that only overlaps another one's holes. An interval live
   across a call (inside one of its ranges, not in a hole) may only take a
   callee-saved register (x19–x28, d8–d15). Allocation hints (argument and return
   registers, and for a phi's incoming value the phi's own register) are honoured
   when the register is free for the whole interval; the copies at loop back edges
   then disappear.
4. Spilling: when no register is free for the whole interval, the register whose
   occupants are cheapest to evict is chosen, where cost is the number of uses and
   definitions weighted by 10^loop depth. If even that costs more than the new
   interval, the new interval is spilled. A spilled value lives in its stack slot
   for its whole life (*spill everywhere*); the emitter reloads it into a scratch
   register at each use.

Replacing the first version (one hull interval per register, spilling the interval
that ends last) changed Lua's `luaV_execute` from 295 spilled values and 3,056
stack loads and stores to 54 and 327. Together with the addressing modes and LICM
added at the same time, the Lua benchmark went from 0.23 s to 0.14 s.

Physical registers appear only in parallel moves next to calls, returns and the
function entry, and no virtual register is live in a caller-saved register across
them (it would be live across the call). x15–x17 and v29–v31 are reserved as
scratch registers for spill reloads, large offsets and breaking cycles in parallel
moves; x18 is never used.

## Frames (`emit.c`)

```
higher addresses   incoming stack arguments        x29 + 16 ...
                   saved x29, x30                  x29
                   saved callee-saved registers    x29 - 16k
                   stack slots for variables
                   spill slots
lower addresses    outgoing stack arguments        sp
```

Spill slots sit next to the outgoing area so their offsets always fit the scaled
12-bit immediate of `ldr`/`str`. A leaf function that needs no stack gets no frame
at all. The epilogue restores sp from x29, so it is correct even after a variable
length array moved sp; such a function addresses its slots through x28, which
holds sp as it was after the prologue. A block that declares a VLA saves sp on
entry and restores it on exit and before `break`/`continue` leave it (not before a
`goto`), so a loop does not grow the stack.

## Determinism and the bootstrap

The headline check (`make bootstrap`) builds ouro three times: stage 1 with the
system compiler, stage 2 with stage 1, stage 3 with stage 2. Stages 2 and 3 are the
same source compiled by the same compiler source, so they must be byte-identical,
and they are. The script also requires that stage 1 and stage 2 emit identical
assembly for every source file, which shows that ouro compiled itself correctly,
not only consistently.

For this to hold, the output must be a pure function of the input. Nothing
iterates over a hash table or orders anything by pointer value; the only `qsort`
calls sort by keys that are unique or tie-broken by numbers assigned in program
order; temporary file names never reach the output; `__DATE__` and `__TIME__` are
fixed unless `SOURCE_DATE_EPOCH` is set. The linker writes the SDK version into
the executable (see below), so binaries are identical on one machine, not across
SDKs.

## Testing

| Layer | What | Where |
|---|---|---|
| Unit | hash map, interning, type conversions, layout, ABI classification, literal types, constant folding (randomised), bitmask immediates (exhaustive) | `tests/unit` |
| Preprocessor | token stream identical to `clang -E` | `tests/pp` |
| Diagnostics | message, line, column, caret | `tests/diag` |
| Optimizer | FileCheck-style patterns over `--dump-ir` | `tests/ir` |
| Code generation | patterns over the assembly | `tests/asm` |
| Programs | output identical to clang's at -O0 and -O1 | `tests/regress` |
| ABI | ouro and clang callers and callees in every combination, 80 signatures per seed | `tests/abi`, `scripts/abi.sh` |
| Random programs | well-defined generated programs, output compared with clang; a line-level reducer | `tests/fuzz` |
| Suites | c-testsuite; Lua 5.4.7 and its test suite (also with Lua's internal assertions); zlib 1.3.1; SQLite 3.50.4 with a SQL workload | `scripts/` |
| Self | three-stage bootstrap; the stage 2 compiler runs the regression, unit/IR and c-testsuite tests | `scripts/bootstrap.sh`, `make stage2-test` |

Bugs these layers found while ouro was written include: a branch emitted before
the instructions computing its condition (only Lua's internal assertions, which
branch on string literal addresses, exposed it); a shift folded into an add whose
operands were later swapped (zlib at -O0); quadratic phi evaluation in SCCP that
took 4.3 s on Lua's interpreter loop; a VLA stack restore placed before the load
it used; and executables linked with platform version 11.0, which made the system
allocator 15% slower than for the same object linked by clang.

Two bugs were undefined behaviour in ouro's own source, and the bootstrap is what
made one of them visible. Lowering a switch enumerated case values with
`for (x = lo; x <= hi; x++)`, which never terminates when `hi` is `INT64_MAX`. The
clang-built stage 1 happened to stop (clang may assume signed overflow does not
happen); the ouro-built stage 2, whose additions wrap, ran off the end of an array
and crashed on a test with `case LONG_MAX:`. Since then `make stage2-test` runs
the regression tests, the unit and IR tests and the c-testsuite with the stage 2
compiler, and a UBSan build of ouro has compiled Lua, zlib, SQLite, the
c-testsuite, its own sources and 100 random programs without a report (it had
found the other bug, a negation of `INT64_MIN` when checking immediates). The random program
generator's first version produced two false alarms, both programs whose result
depends on unspecified evaluation order (a global modified by a callee and read in
the same expression); calls are now generated only as statements.

## Choices not made

- **A Mach-O object writer.** ouro emits assembly and runs `as`. Assembling is now
  most of the time a build takes (about 0.05 s per file, against 0.002–0.07 s for
  ouro itself on Lua's files), so a writer would make builds several times
  faster, but it is a large amount of format work that adds nothing to the
  compiler.
- **Interval splitting and second-chance allocation.** Spill-everywhere with a
  weighted choice is simple and good enough for these programs; splitting would
  mostly help functions as large as Lua's interpreter loop.
- **Inlining, tail calls, loop strength reduction, vectorisation.** Not
  implemented; they explain most of the remaining gap to clang -O2 (see the
  benchmarks in the README).
- **Error recovery.** The first error ends compilation. Recovery would mainly help
  interactive use.
- **Unsupported C11 features**: `_Complex`, `_Atomic` (accepted and ignored as a
  qualifier), `_Thread_local`, multi-dimensional VLAs, `long double` wider than
  `double` (it is not wider on this target), K&R definitions are supported but not
  checked against later prototypes, inline assembly, computed goto.
