#!/usr/bin/env python3
"""Generates calling-convention interoperability tests.

Writes abi.h, callee.c and caller.c into the given directory.  callee.c
defines functions with many different signatures that print every
argument they receive and return a value derived from them; caller.c
calls each one with known values and prints what comes back.  Compiling
caller and callee with different compilers (ouro and clang, in both
directions) and comparing with an all-clang build checks that ouro
follows Apple's arm64 calling convention exactly: register assignment,
stack packing, struct classification, HFAs, indirect results and
variadic arguments.

usage: gen_abi.py OUTDIR [SEED] [COUNT]
"""

import os
import random
import sys

SCALARS = {
    "char": "%d", "signed char": "%d", "unsigned char": "%u", "short": "%d",
    "unsigned short": "%u", "int": "%d", "unsigned": "%u", "long": "%ld",
    "unsigned long": "%lu", "long long": "%lld", "_Bool": "%d",
    "float": "%a", "double": "%a", "long double": "%La",
}

STRUCTS = {
    # name: (definition body, list of (field, printf type))
    "C1": ("char a;", [("a", "int")]),
    "C3": ("char a, b, c;", [("a", "int"), ("b", "int"), ("c", "int")]),
    "SC": ("short a; char b;", [("a", "int"), ("b", "int")]),
    "I1": ("int a;", [("a", "int")]),
    "I3": ("int a, b, c;", [("a", "int"), ("b", "int"), ("c", "int")]),
    "L2": ("long a, b;", [("a", "long"), ("b", "long")]),
    "L3": ("long a, b, c;", [("a", "long"), ("b", "long"), ("c", "long")]),
    "B17": ("char s[17];", [("s[0]", "int"), ("s[8]", "int"), ("s[16]", "int")]),
    "F1": ("float a;", [("a", "double")]),
    "F3": ("float a, b, c;", [("a", "double"), ("b", "double"), ("c", "double")]),
    "F4": ("float a[4];", [("a[0]", "double"), ("a[3]", "double")]),
    "D2": ("double a, b;", [("a", "double"), ("b", "double")]),
    "D4": ("double a, b, c, d;", [("a", "double"), ("d", "double")]),
    "FD": ("float a; double b;", [("a", "double"), ("b", "double")]),
    "CD": ("char a; double b;", [("a", "int"), ("b", "double")]),
    "NF": ("struct { float x, y; } p; float z;", [("p.x", "double"), ("p.y", "double"), ("z", "double")]),
    "UI": ("union { int i; float f; } u;", [("u.i", "int")]),
    "BF": ("unsigned a : 3; int b : 9; unsigned c : 20;", [("a", "int"), ("b", "int"), ("c", "int")]),
    "PS": ("char *p; int n;", [("n", "int")]),
}

FMT = {"int": "%d", "long": "%ld", "double": "%a"}


def value(r, ty):
    if ty == "_Bool":
        return str(r.randint(0, 1))
    if ty in ("float", "double", "long double"):
        v = r.choice([0.5, -1.25, 3.0e10, 1.0 / 3, -0.0, 7.0, 123.456])
        suffix = {"float": "f", "double": "", "long double": "L"}[ty]
        return repr(v) + suffix
    if ty in ("char", "signed char"):
        return str(r.randint(-128, 127))
    if ty == "unsigned char":
        return str(r.randint(0, 255))
    if ty == "short":
        return str(r.randint(-32768, 32767))
    if ty == "unsigned short":
        return str(r.randint(0, 65535))
    if ty == "int":
        return str(r.randint(-2**31 + 1, 2**31 - 1))
    if ty == "unsigned":
        return str(r.randint(0, 2**32 - 1)) + "u"
    if ty in ("long", "long long"):
        return str(r.randint(-2**63 + 1, 2**63 - 1)) + "L"
    if ty == "unsigned long":
        return str(r.randint(0, 2**64 - 1)) + "UL"
    raise ValueError(ty)


def struct_init(r, name):
    ints = lambda lo, hi: str(r.randint(lo, hi))
    flt = lambda: repr(r.choice([0.5, 2.25, -8.0, 1e-3, 1.0 / 7]))
    return {
        "C1": f"{{{ints(-128, 127)}}}",
        "C3": f"{{{ints(-128, 127)}, {ints(-128, 127)}, {ints(-128, 127)}}}",
        "SC": f"{{{ints(-30000, 30000)}, {ints(-128, 127)}}}",
        "I1": f"{{{ints(-10**9, 10**9)}}}",
        "I3": f"{{{ints(-10**9, 10**9)}, {ints(-10**9, 10**9)}, {ints(-10**9, 10**9)}}}",
        "L2": f"{{{ints(-10**18, 10**18)}L, {ints(-10**18, 10**18)}L}}",
        "L3": f"{{{ints(-10**18, 10**18)}L, {ints(-10**18, 10**18)}L, {ints(-10**18, 10**18)}L}}",
        "B17": f"{{{{{', '.join(ints(-128, 127) for _ in range(17))}}}}}",
        "F1": f"{{{flt()}f}}",
        "F3": f"{{{flt()}f, {flt()}f, {flt()}f}}",
        "F4": f"{{{{{flt()}f, {flt()}f, {flt()}f, {flt()}f}}}}",
        "D2": f"{{{flt()}, {flt()}}}",
        "D4": f"{{{flt()}, {flt()}, {flt()}, {flt()}}}",
        "FD": f"{{{flt()}f, {flt()}}}",
        "CD": f"{{{ints(-128, 127)}, {flt()}}}",
        "NF": f"{{{{{flt()}f, {flt()}f}}, {flt()}f}}",
        "UI": f"{{{{{ints(-10**9, 10**9)}}}}}",
        "BF": f"{{{ints(0, 7)}, {ints(-256, 255)}, {ints(0, 2**20 - 1)}}}",
        "PS": f"{{0, {ints(-1000, 1000)}}}",
    }[name]


def print_arg(name, ty):
    """C statements printing argument `name` of type `ty`."""
    if ty in SCALARS:
        cast = {"float": "(double)", "_Bool": "(int)", "char": "(int)", "signed char": "(int)",
                "unsigned char": "(unsigned)", "short": "(int)", "unsigned short": "(unsigned)"}.get(ty, "")
        return f'printf(" {SCALARS[ty]}", {cast}{name});'
    s = ty.split()[1]
    parts = []
    for field, pty in STRUCTS[s][1]:
        cast = {"int": "(int)", "long": "(long)", "double": "(double)"}[pty]
        parts.append(f'printf(" {FMT[pty]}", {cast}{name}.{field});')
    return " ".join(parts)


def main():
    out = sys.argv[1]
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 80
    r = random.Random(seed)
    types = list(SCALARS) + [f"struct {s}" for s in STRUCTS]
    ret_types = ["void", "char", "unsigned short", "int", "long", "float", "double", "long double"] + \
        [f"struct {s}" for s in STRUCTS]

    h = ["/* generated by tests/abi/gen_abi.py */", "#include <stdio.h>", "#include <stdarg.h>", ""]
    for s, (body, _) in STRUCTS.items():
        h.append(f"struct {s} {{ {body} }};")
    h.append("")
    callee = ['#include "abi.h"', ""]
    caller = ['#include "abi.h"', "", "int main(void) {"]

    for n in range(count):
        nargs = r.randint(0, 14)
        params = [r.choice(types) for _ in range(nargs)]
        ret = r.choice(ret_types)
        variadic = nargs > 0 and r.random() < 0.25
        nfixed = r.randint(1, nargs) if variadic else nargs
        fixed = params[:nfixed]
        plist = ", ".join(f"{t} a{i}" for i, t in enumerate(fixed)) or "void"
        if variadic:
            plist += ", ..."
        h.append(f"{ret} f{n}({plist});")

        # callee
        callee.append(f"{ret} f{n}({plist}) {{")
        callee.append(f'  printf("f{n}:");')
        for i, t in enumerate(fixed):
            callee.append("  " + print_arg(f"a{i}", t))
        if variadic:
            callee.append("  va_list ap;")
            callee.append(f"  va_start(ap, a{nfixed - 1});")
            for i, t in enumerate(params[nfixed:], start=nfixed):
                # default argument promotions apply to variadic arguments
                pt = {"char": "int", "signed char": "int", "unsigned char": "int", "short": "int",
                      "unsigned short": "int", "_Bool": "int", "float": "double"}.get(t, t)
                callee.append(f"  {pt} v{i} = va_arg(ap, {pt});")
                callee.append("  " + print_arg(f"v{i}", pt))
            callee.append("  va_end(ap);")
        callee.append('  printf("\\n");')
        if ret == "void":
            pass
        elif ret.startswith("struct"):
            callee.append(f"  {ret} r = {struct_init(r, ret.split()[1])};")
            callee.append("  return r;")
        else:
            callee.append(f"  return ({ret}){value(r, ret)};")
        callee.append("}")
        callee.append("")

        # caller
        args = []
        for t in params:
            if t.startswith("struct"):
                args.append(f"({t}){struct_init(r, t.split()[1])}")
            else:
                args.append(f"({t}){value(r, t)}")
        call = f"f{n}({', '.join(args)})"
        caller.append("  {")
        if ret == "void":
            caller.append(f"    {call};")
        else:
            caller.append(f"    {ret} res = {call};")
            caller.append(f'    printf("ret{n}:");')
            caller.append("    " + print_arg("res", ret))
            caller.append('    printf("\\n");')
        caller.append("  }")
    caller.append("  return 0;")
    caller.append("}")

    with open(os.path.join(out, "abi.h"), "w") as f:
        f.write("\n".join(h) + "\n")
    with open(os.path.join(out, "callee.c"), "w") as f:
        f.write("\n".join(callee) + "\n")
    with open(os.path.join(out, "caller.c"), "w") as f:
        f.write("\n".join(caller) + "\n")


if __name__ == "__main__":
    main()
