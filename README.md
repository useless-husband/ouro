# ouro

**A self-hosting C11 compiler for arm64 macOS, written in C.**

ouro compiles its own source code, and the compiler produced that way compiles
the source again to a byte-identical binary (`make bootstrap`). It has its own
preprocessor, parser and type checker, lowers to an SSA intermediate
representation, allocates registers with linear scan and emits arm64 assembly
following Apple's calling convention. It drives the system assembler and linker.

Status: under active development. Current measured results:

- `make bootstrap`: stage 2 and stage 3 are byte-identical.
- `make c-testsuite`: 219 of 220 [c-testsuite](https://github.com/c-testsuite/c-testsuite)
  tests pass; the remaining one needs variable length arrays.

## Build and test

```sh
make            # build/ouro, built with the system compiler
make test       # regression tests compared against clang
make bootstrap  # three-stage self-compilation check
make c-testsuite
```

## License

[MIT](LICENSE)
