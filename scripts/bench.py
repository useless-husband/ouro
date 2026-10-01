#!/usr/bin/env python3
"""Benchmarks: generated-code speed and compiler speed.

Generated code: every program in tests/bench is built with clang -O0,
clang -O2, ouro -O0 and ouro -O1; all builds must print the same output;
each is run RUNS times and the median wall-clock time is reported.  Lua
5.4.7 (built by scripts/lua.sh) is also built with clang at both levels
and timed running tests/bench/fib.lua.

Compiler speed: time to compile Lua's 33 source files to assembly, and
the time for ouro to compile itself, against clang at -O0 and -O2.

usage: scripts/bench.py [--runs N] [--markdown FILE]
"""

import argparse
import os
import platform
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OURO = os.path.join(ROOT, "build", "ouro")
BENCH = os.path.join(ROOT, "tests", "bench")
ARGS = {"nbody.c": ["10000000"], "matmul.c": ["600"], "fannkuch.c": ["10"],
        "binarytrees.c": ["18"], "sieve.c": ["50000000", "4"]}
BUILDS = [("clang -O0", ["clang", "-O0", "-w"]), ("clang -O2", ["clang", "-O2", "-w"]),
          ("ouro -O0", [OURO, "-O0", "-w"]), ("ouro -O1", [OURO, "-O1", "-w"])]


def timed(cmd, runs, cwd=None):
    times, out = [], None
    for _ in range(runs):
        t0 = time.perf_counter()
        p = subprocess.run(cmd, capture_output=True, cwd=cwd)
        times.append(time.perf_counter() - t0)
        if p.returncode != 0:
            sys.exit(f"{' '.join(cmd)} failed: {p.stderr.decode()[:500]}")
        out = p.stdout
    return statistics.median(times), out


def code_speed(runs, tmp):
    rows = []
    for prog in sorted(ARGS):
        src = os.path.join(BENCH, prog)
        times, outs = {}, set()
        for name, cc in BUILDS:
            exe = os.path.join(tmp, prog + "." + name.replace(" ", ""))
            subprocess.run(cc + ["-o", exe, src, "-lm"], check=True)
            t, out = timed([exe] + ARGS[prog], runs)
            times[name] = t
            outs.add(out)
        if len(outs) != 1:
            sys.exit(f"{prog}: builds disagree on the output")
        rows.append((prog, times))
    return rows


def lua_speed(runs, tmp):
    lua_src = os.path.join(ROOT, "build", "lua", "lua-5.4.7", "src")
    if not os.path.isdir(lua_src):
        return None
    times, outs = {}, set()
    for name, cc in BUILDS:
        objs = []
        d = os.path.join(tmp, "lua-" + name.replace(" ", ""))
        os.makedirs(d, exist_ok=True)
        for f in sorted(os.listdir(lua_src)):
            if not f.endswith(".c") or f == "luac.c":
                continue
            o = os.path.join(d, f[:-2] + ".o")
            subprocess.run(cc + ["-DLUA_USE_MACOSX", "-c", "-o", o, os.path.join(lua_src, f)],
                           check=True)
            objs.append(o)
        exe = os.path.join(d, "lua")
        subprocess.run(["clang", "-o", exe] + objs, check=True)
        t, out = timed([exe, os.path.join(BENCH, "fib.lua")], runs)
        times[name] = t
        outs.add(out)
    if len(outs) != 1:
        sys.exit("lua: builds disagree on the output")
    return ("lua fib.lua", times)


def compile_speed(runs, tmp):
    """Seconds to compile a set of files to assembly, sequentially."""
    lua_src = os.path.join(ROOT, "build", "lua", "lua-5.4.7", "src")
    sets = []
    if os.path.isdir(lua_src):
        files = [os.path.join(lua_src, f) for f in sorted(os.listdir(lua_src))
                 if f.endswith(".c") and f != "luac.c"]
        sets.append(("Lua 5.4.7 (%d files, %d lines)" % (len(files), count_lines(files)),
                     files, ["-DLUA_USE_MACOSX"]))
    own = [os.path.join(ROOT, "src", f) for f in sorted(os.listdir(os.path.join(ROOT, "src")))
           if f.endswith(".c")]
    sets.append(("ouro itself (%d files, %d lines)" % (len(own), count_lines(own)), own, []))
    rows = []
    for label, files, extra in sets:
        times = {}
        for name, cc in BUILDS:
            def run_all():
                for f in files:
                    subprocess.run(cc + extra + ["-S", "-o", os.devnull, f], check=True)
            samples = []
            for _ in range(runs):
                t0 = time.perf_counter()
                run_all()
                samples.append(time.perf_counter() - t0)
            times[name] = statistics.median(samples)
        rows.append((label, times))
    return rows


def count_lines(files):
    return sum(sum(1 for _ in open(f, errors="replace")) for f in files)


def table(title, rows, unit="s"):
    names = [b[0] for b in BUILDS]
    lines = [f"| {title} | " + " | ".join(names) + " | ouro -O1 vs clang -O0 | ouro -O1 vs clang -O2 |",
             "|---|" + "---:|" * (len(names) + 2)]
    for label, times in rows:
        cells = [f"{times[n]:.3f}" for n in names]
        r0 = times["clang -O0"] / times["ouro -O1"]
        r2 = times["clang -O2"] / times["ouro -O1"]
        lines.append(f"| {label} | " + " | ".join(cells) + f" | {r0:.2f}x | {r2:.2f}x |")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--markdown")
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        rows = code_speed(a.runs, tmp)
        lua = lua_speed(a.runs, tmp)
        if lua:
            rows.append(lua)
        comp = compile_speed(max(1, a.runs // 2), tmp)
    machine = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True,
                             text=True).stdout.strip()
    header = (f"Machine: {machine}, macOS {platform.mac_ver()[0]}; "
              f"{subprocess.run(['clang', '--version'], capture_output=True, text=True).stdout.splitlines()[0]}. "
              f"Median of {a.runs} runs, wall-clock seconds; ratios above 1 mean ouro is faster.")
    md = "\n\n".join([header,
                      table("program (run time, s)", rows),
                      table("compile to assembly (s)", comp)])
    print(md)
    if a.markdown:
        with open(a.markdown, "w") as f:
            f.write(md + "\n")


if __name__ == "__main__":
    main()
