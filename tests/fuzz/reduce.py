#!/usr/bin/env python3
"""Shrinks a C program that ouro and clang disagree on.

Repeatedly deletes single lines (statements, declarations, case labels)
from the program and keeps a deletion when the result still compiles
with clang, still has no undefined behaviour according to UBSan, and
ouro still prints something different.  Line-level deletion suits the
programs written by gen.py, which put one statement per line.

usage: reduce.py PROGRAM.c OURO [-O0|-O1] > reduced.c
"""

import os
import subprocess
import sys
import tempfile


def run(cmd, timeout=20):
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout)
        return p.returncode, p.stdout
    except subprocess.TimeoutExpired:
        return None, b""


def interesting(src, ouro, opt, tmp):
    c = os.path.join(tmp, "r.c")
    with open(c, "w") as f:
        f.write(src)
    ref = os.path.join(tmp, "ref")
    if run(["clang", "-w", "-ffp-contract=off", "-fsanitize=undefined",
            "-fno-sanitize-recover=undefined", "-o", ref, c])[0] != 0:
        return False
    rc, ref_out = run([ref])
    if rc != 0:
        return False
    t = os.path.join(tmp, "t")
    if run([ouro, "-w", opt, "-o", t, c])[0] != 0:
        return False
    rc2, out = run([t])
    return rc2 is None or rc2 != 0 or out != ref_out


def main():
    path, ouro = sys.argv[1], os.path.abspath(sys.argv[2])
    opt = sys.argv[3] if len(sys.argv) > 3 else "-O1"
    lines = open(path).read().split("\n")
    with tempfile.TemporaryDirectory() as tmp:
        if not interesting("\n".join(lines), ouro, opt, tmp):
            sys.exit("the input is not interesting (no difference)")
        changed = True
        while changed:
            changed = False
            i = len(lines) - 1
            while i >= 0:
                if lines[i].strip() in ("", "}", "{") or lines[i].startswith("#") or "return" in lines[i]:
                    i -= 1
                    continue
                cand = lines[:i] + lines[i + 1:]
                if interesting("\n".join(cand), ouro, opt, tmp):
                    lines = cand
                    changed = True
                i -= 1
    sys.stdout.write("\n".join(lines))


if __name__ == "__main__":
    main()
