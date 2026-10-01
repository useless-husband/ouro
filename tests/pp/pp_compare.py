#!/usr/bin/env python3
"""Compares two preprocessor outputs token by token.

Whitespace between tokens is ignored, whitespace inside string and
character literals is not (it is how stringification is observed).

usage: pp_compare.py A B
"""
import re
import sys

TOKEN = re.compile(r'''("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[A-Za-z_]\w*|\.?\d(?:[eEpP][+-]|[\w.])*|\S)''')


def tokens(path):
    out = []
    for line in open(path):
        if line.startswith("#"):
            continue  # line markers
        out.extend(TOKEN.findall(line))
    return out


a, b = tokens(sys.argv[1]), tokens(sys.argv[2])
if a != b:
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            print(f"first difference at token {i}: {x!r} vs {y!r}")
            print("context:", " ".join(a[max(0, i - 8):i + 3]))
            break
    else:
        print(f"lengths differ: {len(a)} vs {len(b)}")
    sys.exit(1)
