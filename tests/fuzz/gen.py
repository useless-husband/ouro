#!/usr/bin/env python3
"""Generates a random, well-defined C program for differential testing.

The program uses integers of every width, arrays, structs passed and
returned by value, pointers, loops, switches, gotos-free control flow and
calls between functions.  Undefined behaviour is avoided the way Csmith
does it: signed arithmetic goes through guarded helper functions, shift
counts are masked, divisions check for zero and overflow, array indices
are reduced modulo the length, and every loop has a constant bound.  The
program prints intermediate values and a checksum of its globals, so any
difference from clang's output is a miscompilation by one of the two.

usage: gen.py SEED > prog.c
"""

import random
import sys

INT_TYPES = [
    ("signed char", 8, True), ("unsigned char", 8, False),
    ("short", 16, True), ("unsigned short", 16, False),
    ("int", 32, True), ("unsigned", 32, False),
    ("long", 64, True), ("unsigned long", 64, False),
]
TYPE_NAMES = [t[0] for t in INT_TYPES]
INFO = {t[0]: t for t in INT_TYPES}

HEADER = r"""
#include <stdio.h>
#include <limits.h>
#include <stdint.h>

static int32_t s32_add(int32_t a, int32_t b) {
  return ((b > 0 && a > INT32_MAX - b) || (b < 0 && a < INT32_MIN - b)) ? a : a + b;
}
static int32_t s32_sub(int32_t a, int32_t b) {
  return ((b < 0 && a > INT32_MAX + b) || (b > 0 && a < INT32_MIN + b)) ? a : a - b;
}
static int32_t s32_mul(int32_t a, int32_t b) {
  int64_t r = (int64_t)a * b;
  return (r > INT32_MAX || r < INT32_MIN) ? a : (int32_t)r;
}
static int32_t s32_div(int32_t a, int32_t b) {
  return (b == 0 || (a == INT32_MIN && b == -1)) ? a : a / b;
}
static int32_t s32_mod(int32_t a, int32_t b) {
  return (b == 0 || (a == INT32_MIN && b == -1)) ? a : a % b;
}
static int64_t s64_add(int64_t a, int64_t b) {
  return ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) ? a : a + b;
}
static int64_t s64_sub(int64_t a, int64_t b) {
  return ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) ? a : a - b;
}
static int64_t s64_mul(int64_t a, int64_t b) {
  if (a == 0 || b == 0) return 0;
  if ((a > 0 && b > 0 && a > INT64_MAX / b) || (a < 0 && b < 0 && a < INT64_MAX / b) ||
      (a > 0 && b < 0 && b < INT64_MIN / a) || (a < 0 && b > 0 && a < INT64_MIN / b))
    return a;
  return a * b;
}
static int64_t s64_div(int64_t a, int64_t b) {
  return (b == 0 || (a == INT64_MIN && b == -1)) ? a : a / b;
}
static int64_t s64_mod(int64_t a, int64_t b) {
  return (b == 0 || (a == INT64_MIN && b == -1)) ? a : a % b;
}
static uint64_t u_div(uint64_t a, uint64_t b) { return b == 0 ? a : a / b; }
static uint64_t u_mod(uint64_t a, uint64_t b) { return b == 0 ? a : a % b; }

static uint64_t checksum;
static void mix(uint64_t v) { checksum = (checksum ^ v) * 0x100000001b3ULL + 7; }

struct S { int a; unsigned char b; long c; short d[3]; };
struct P { long x, y; };
struct F { double u, v; };
"""


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.out = []
        self.globals = []   # (name, type)
        self.arrays = []    # (name, type, length)
        self.funcs = []     # (name, return type, [param types])
        self.indent = 1
        self.loop_vars = []
        self.loop_depth = 0
        self.readonly = set()

    def emit(self, s):
        self.out.append("  " * self.indent + s)

    def const(self, ty):
        name, bits, signed = INFO[ty]
        r = self.r.random()
        if r < 0.3:
            v = self.r.randint(-5 if signed else 0, 10)
        elif r < 0.6:
            v = self.r.randint(0, 1000)
        else:
            lo = -(1 << (bits - 1)) if signed else 0
            hi = (1 << (bits - 1)) - 1 if signed else (1 << bits) - 1
            v = self.r.choice([lo, hi, self.r.randint(lo, hi)])
        if bits == 64:
            if signed:
                return "(-9223372036854775807L - 1)" if v == -(1 << 63) else f"{v}L"
            return f"{v}UL"
        if bits == 32 and not signed:
            return f"{v}U"
        if bits == 32 and v == -(1 << 31):
            return "(-2147483647 - 1)"
        return f"(({ty}){v})"

    def var(self, scope):
        cands = list(scope) + self.globals
        return self.r.choice(cands) if cands else None

    def expr(self, ty, scope, depth):
        """An expression of type `ty` (always written with a cast)."""
        r = self.r.random()
        if depth <= 0 or r < 0.2:
            v = self.var(scope)
            if v and self.r.random() < 0.75:
                return f"(({ty}){v[0]})"
            if self.arrays and self.r.random() < 0.3:
                a = self.r.choice(self.arrays)
                return f"(({ty}){a[0]}[{self.index(a, scope, 0)}])"
            return self.const(ty)
        name, bits, signed = INFO[ty]
        a = self.expr(ty, scope, depth - 1)
        b = self.expr(ty, scope, depth - 1)
        # Unsigned types narrower than int promote to int, where + - *
        # could overflow: compute them in unsigned int instead.
        ua, ub = (f"(unsigned){a}", f"(unsigned){b}") if (not signed and bits < 32) else (a, b)
        k = self.r.randrange(13)
        if k == 0:
            if signed and bits <= 32:
                return f"(({ty})s32_add({a}, {b}))"
            if signed:
                return f"(({ty})s64_add({a}, {b}))"
            return f"(({ty})({ua} + {ub}))"
        if k == 1:
            if signed and bits <= 32:
                return f"(({ty})s32_sub({a}, {b}))"
            if signed:
                return f"(({ty})s64_sub({a}, {b}))"
            return f"(({ty})({ua} - {ub}))"
        if k == 2:
            if signed and bits <= 32:
                return f"(({ty})s32_mul({a}, {b}))"
            if signed:
                return f"(({ty})s64_mul({a}, {b}))"
            return f"(({ty})({ua} * {ub}))"
        if k == 3:
            if signed:
                f = "s32" if bits <= 32 else "s64"
                return f"(({ty}){f}_{self.r.choice(['div', 'mod'])}({a}, {b}))"
            return f"(({ty})u_{self.r.choice(['div', 'mod'])}({a}, {b}))"
        if k == 4:
            op = self.r.choice(["&", "|", "^"])
            return f"(({ty})({a} {op} {b}))"
        if k == 5:
            # shifts: the left operand is made unsigned for <<, the count masked
            w = 32 if bits <= 32 else 64
            ut = "unsigned" if w == 32 else "unsigned long"
            if self.r.random() < 0.5:
                return f"(({ty})(({ut}){a} << ({b} & {w - 1})))"
            return f"(({ty})({a} >> ({b} & {w - 1})))"
        if k == 6:
            op = self.r.choice(["<", "<=", ">", ">=", "==", "!="])
            other = self.r.choice(TYPE_NAMES)
            return f"(({ty})({a} {op} {self.expr(other, scope, depth - 1)}))"
        if k == 7:
            op = self.r.choice(["&&", "||"])
            return f"(({ty})({a} {op} {b}))"
        if k == 8:
            c = self.expr(self.r.choice(TYPE_NAMES), scope, depth - 1)
            return f"(({ty})({c} ? {a} : {b}))"
        if k == 9:
            op = self.r.choice(["~", "!"])
            if signed and op == "-":
                return a
            return f"(({ty})({op}{a}))"
        if k == 10:
            other = self.r.choice(TYPE_NAMES)
            return f"(({ty}){self.expr(other, scope, depth - 1)})"
        if k == 12:
            # floating point detour (exactly representable small values)
            # through long first: an out-of-range double to int conversion is undefined
            return f"(({ty})(long)(((double)({a} & 1023) * 0.5) + (double)({b} & 255)))"
        return a

    def index(self, arr, scope, depth):
        name, ty, n = arr
        e = self.expr("unsigned", scope, depth)
        return f"({e} % {n}u)"

    def lvalue(self, scope):
        r = self.r.random()
        if self.arrays and r < 0.25:
            a = self.r.choice(self.arrays)
            return (f"{a[0]}[{self.index(a, scope, 1)}]", a[1])
        cands = [v for v in list(scope) + self.globals if v[0] not in self.readonly]
        v = self.r.choice(cands) if cands else self.globals[0]
        return v

    def stmt(self, scope, depth):
        r = self.r.random()
        if depth <= 0 or r < 0.45:
            lv, ty = self.lvalue(scope)
            if self.r.random() < 0.3 and not INFO[ty][2] and INFO[ty][1] >= 32:
                op = self.r.choice(["+=", "-=", "*=", "^=", "|=", "&="])
                self.emit(f"{lv} {op} {self.expr(ty, scope, 2)};")
            else:
                self.emit(f"{lv} = {self.expr(ty, scope, 3)};")
            return
        if r < 0.6:
            self.emit(f"if ({self.expr(self.r.choice(TYPE_NAMES), scope, 2)}) {{")
            self.block(scope, depth - 1)
            if self.r.random() < 0.5:
                self.emit("} else {")
                self.block(scope, depth - 1)
            self.emit("}")
            return
        if r < 0.72:
            iv = f"i{len(self.loop_vars)}"
            n = self.r.randint(1, 5)
            self.emit(f"for (int {iv} = 0; {iv} < {n}; {iv}++) {{")
            self.loop_vars.append(iv)
            self.readonly.add(iv)
            self.loop_depth += 1
            self.block(scope + [(iv, "int")], depth - 1)
            self.loop_depth -= 1
            self.readonly.discard(iv)
            self.loop_vars.pop()
            self.emit("}")
            return
        if r < 0.82:
            self.emit(f"switch ({self.expr('int', scope, 2)} & 7) {{")
            for c in sorted(self.r.sample(range(8), self.r.randint(1, 4))):
                self.emit(f"case {c}:")
                self.indent += 1
                self.stmt(scope, depth - 1)
                if self.r.random() < 0.7:
                    self.emit("break;")
                self.indent -= 1
            self.emit("default:")
            self.indent += 1
            self.stmt(scope, depth - 1)
            self.indent -= 1
            self.emit("}")
            return
        if r < 0.86:
            self.emit(f"mix((uint64_t){self.expr(self.r.choice(TYPE_NAMES), scope, 3)});")
            return
        if r < 0.9 and self.funcs and self.loop_depth <= 1:
            # Calls only appear as whole statements with side-effect-free
            # arguments: C leaves the order of evaluation inside an
            # expression unspecified, and the callees modify globals.
            f = self.r.choice(self.funcs)
            args = ", ".join(self.expr(p, scope, 2) for p in f[2])
            locals_ = [v for v in scope if v[0] not in self.readonly and v[0][0] in "lp"]
            if locals_ and self.r.random() < 0.6:
                name, ty = self.r.choice(locals_)
                self.emit(f"{name} = ({ty}){f[0]}({args});")
            else:
                self.emit(f"mix((uint64_t){f[0]}({args}));")
            return
        # struct work
        self.emit("{")
        self.indent += 1
        self.emit(f"struct S s = {{ {self.expr('int', scope, 1)}, {self.expr('unsigned char', scope, 1)}, "
                  f"{self.expr('long', scope, 1)}, {{ {self.expr('short', scope, 1)}, 2, 3 }} }};")
        self.emit(f"struct P p = mkp(s, {self.expr('long', scope, 1)});")
        self.emit("mix((uint64_t)(p.x ^ p.y) + s.d[1]);")
        self.indent -= 1
        self.emit("}")

    def block(self, scope, depth):
        self.indent += 1
        scope = list(scope)
        for _ in range(self.r.randint(1, 3)):
            if self.r.random() < 0.3:
                ty = self.r.choice(TYPE_NAMES)
                name = f"l{self.r.randrange(1 << 30)}"
                self.emit(f"{ty} {name} = {self.expr(ty, scope, 2)};")
                scope.append((name, ty))
            self.stmt(scope, depth)
        self.indent -= 1

    def function(self, idx):
        ret = self.r.choice(TYPE_NAMES)
        params = [self.r.choice(TYPE_NAMES) for _ in range(self.r.randint(0, 6))]
        name = f"f{idx}"
        plist = ", ".join(f"{t} p{i}" for i, t in enumerate(params)) or "void"
        self.out.append(f"static {ret} {name}({plist}) {{")
        scope = [(f"p{i}", t) for i, t in enumerate(params)]
        self.indent = 1
        for _ in range(self.r.randint(2, 6)):
            self.stmt(scope, 3)
        self.emit(f"return {self.expr(ret, scope, 3)};")
        self.out.append("}")
        self.out.append("")
        self.funcs.append((name, ret, params))

    def program(self):
        out = self.out
        out.append(HEADER)
        for i in range(self.r.randint(3, 8)):
            ty = self.r.choice(TYPE_NAMES)
            out.append(f"static {ty} g{i} = {self.const(ty)};")
            self.globals.append((f"g{i}", ty))
        for i in range(self.r.randint(1, 3)):
            ty = self.r.choice(TYPE_NAMES)
            n = self.r.randint(1, 9)
            vals = ", ".join(self.const(ty) for _ in range(n))
            out.append(f"static {ty} a{i}[{n}] = {{{vals}}};")
            self.arrays.append((f"a{i}", ty, n))
        out.append("")
        out.append("static struct P mkp(struct S s, long k) {")
        out.append("  struct P p = { s.c ^ k, (long)s.a + s.b + s.d[0] };")
        out.append("  return p;")
        out.append("}")
        out.append("")
        for i in range(self.r.randint(2, 7)):
            self.function(i)
        out.append("int main(void) {")
        self.indent = 1
        for f in self.funcs:
            args = ", ".join(self.const(p) for p in f[2])
            self.emit(f"mix((uint64_t){f[0]}({args}));")
            self.emit(f'printf("{f[0]} %llx\\n", (unsigned long long)checksum);')
        for name, ty in self.globals:
            self.emit(f"mix((uint64_t){name});")
        for name, ty, n in self.arrays:
            self.emit(f"for (int i = 0; i < {n}; i++) mix((uint64_t){name}[i]);")
        self.emit('printf("checksum %llx\\n", (unsigned long long)checksum);')
        self.emit("return 0;")
        out.append("}")
        return "\n".join(out) + "\n"


if __name__ == "__main__":
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    sys.stdout.write(Gen(seed).program())
