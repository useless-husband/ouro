#!/usr/bin/env python3
"""A small FileCheck: verifies --dump-ir or assembly output against patterns.

Directives in the test source:
  // CHECK: text        the next line containing `text`, in order
  // CHECK-NOT: text    no line containing `text` between the surrounding
                        CHECK matches (or in the whole function section
                        if it comes first)
  // CHECK-LABEL: text  like CHECK, and starts a new section
  // CHECK-COUNT-N: text  exactly N lines containing `text` in the section
Text is matched literally except inside {{...}}, which is a regular
expression.  A section ends where the next function begins.

usage: filecheck.py TEST.c OUTPUT
"""
import re
import sys

test, output = sys.argv[1], sys.argv[2]
lines = open(output).read().split("\n")


def pattern(text):
    parts = re.split(r"\{\{(.*?)\}\}", text)
    return re.compile("".join(re.escape(p) if i % 2 == 0 else p for i, p in enumerate(parts)))


def has(text, line):
    return pattern(text).search(line) is not None


def is_function_start(line):
    return line.startswith("function ") or re.match(r"^_[A-Za-z0-9_.$]+:$", line) is not None

directives = []
for line in open(test):
    m = re.match(r"\s*// (CHECK(?:-NOT|-LABEL|-COUNT-\d+)?): (.*)$", line)
    if m:
        directives.append((m.group(1), m.group(2).strip()))

# Split into sections at CHECK-LABEL.
sections = []
for kind, text in directives:
    if kind == "CHECK-LABEL" or not sections:
        sections.append([])
    sections[-1].append((kind, text))

pos = 0
ok = True
for sec in sections:
    # A section spans from its label to the next label's match.
    start = pos
    if sec[0][0] == "CHECK-LABEL":
        while pos < len(lines) and not has(sec[0][1], lines[pos]):
            pos += 1
        if pos == len(lines):
            print(f"{test}: CHECK-LABEL not found: {sec[0][1]}")
            sys.exit(1)
        start = pos
        pos += 1
    end = len(lines)
    for j in range(pos, len(lines)):
        if is_function_start(lines[j]) and j > start:
            end = j
            break
    nots = []
    for kind, text in sec:
        if kind == "CHECK-LABEL":
            continue
        if kind == "CHECK-NOT":
            nots.append(text)
            continue
        if kind.startswith("CHECK-COUNT-"):
            n = int(kind.rsplit("-", 1)[1])
            got = sum(1 for l in lines[start:end] if has(text, l))
            if got != n:
                print(f"{test}: expected {n} lines with '{text}', found {got}")
                ok = False
            continue
        j = pos
        while j < end and not has(text, lines[j]):
            j += 1
        if j == end:
            print(f"{test}: CHECK not found: {text}")
            ok = False
            break
        for t in nots:
            for l in lines[pos:j]:
                if has(t, l):
                    print(f"{test}: CHECK-NOT matched: {t}\n    in: {l}")
                    ok = False
        nots = []
        pos = j + 1
    for t in nots:
        for l in lines[pos:end]:
            if has(t, l):
                print(f"{test}: CHECK-NOT matched: {t}\n    in: {l}")
                ok = False
    pos = end
sys.exit(0 if ok else 1)
