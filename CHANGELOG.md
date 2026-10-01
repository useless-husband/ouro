# Changelog

All notable changes to ouro are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added
- Preprocessor, parser and type checker for a large C11 subset.
- SSA intermediate representation, linear-scan register allocation and an
  arm64 back end following Apple's calling convention.
- Own libc headers for arm64 macOS.
- Three-stage bootstrap check, clang-differential regression tests and a
  c-testsuite runner.
