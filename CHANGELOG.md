# Changelog

All notable changes to ouro are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-10-01

First version.

### Compiler
- Preprocessor with Prosser hidesets, its own `#if` evaluator, include guard
  detection, `#pragma once`, `push_macro`/`pop_macro`, `__VA_OPT__`, `-E`.
- Parser and type checker for C11 without `_Complex`, `_Thread_local` and atomics:
  all integer and floating types, structs, unions, bit-fields, enums, typedefs,
  function pointers, variadic functions, designated initializers, compound
  literals, `_Generic`, `_Static_assert`, `_Alignas`/`_Alignof`, K&R definitions,
  one-dimensional variable length arrays, GNU statement expressions and `typeof`.
- SSA IR with mem2reg, SCCP, combine, CSE, redundant load elimination, LICM, DCE
  and CFG cleanup at `-O1`.
- arm64 instruction selection, linear-scan register allocation on live ranges
  with holes, Apple's arm64 calling convention.
- Driver running `as` and `ld`; `-shared` for dynamic libraries; `--dump-ir`,
  `--dump-mir`, `--time`.
- Own C library headers for macOS.

### Verification
- Three-stage bootstrap with byte-identical stage 2 and stage 3; the stage 2
  compiler also passes the regression, unit/IR and c-testsuite tests.
- c-testsuite 220/220 at `-O0` and `-O1`; Lua 5.4.7 and its test suite; zlib
  1.3.1; SQLite 3.50.4 with a SQL workload; calling-convention tests against
  clang; seeded random program tests; unit, preprocessor, diagnostic, IR and
  assembly tests; ouro built with UBSan over all of the above.
