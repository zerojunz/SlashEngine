#!/usr/bin/env python3
"""Temporary: runs each test case of the test files that draw with OpenGL in a process of its
own and says after which ones macOS's CVMCompiler (the software OpenGL renderer's shader
compiler) is still running, killing it each time so the next case starts clean.

    find-gl-compile-hang.py <slash_tests> <tests folder>
"""
import pathlib
import re
import subprocess
import sys
import time

program, folder = sys.argv[1], pathlib.Path(sys.argv[2])
files = sorted(p.name for p in folder.glob("test_*.cpp")
               if re.search(r"drawing_devices|hardware_devices|gl_device", p.read_text()))
work = pathlib.Path(program).parent


def compilers():
    out = subprocess.run(["ps", "-Ao", "pid,pcpu,etime,rss,comm"], capture_output=True,
                         text=True).stdout
    return [line.split() for line in out.splitlines() if "CVMCompiler" in line]


for name in files:
    listed = subprocess.run([program, "-ltc", f"--source-file=*{name}"], capture_output=True,
                            text=True, cwd=work).stdout.splitlines()
    cases = [line for line in listed if line and not line.startswith(("[doctest]", "====="))]
    for case in cases:
        pattern = case.replace("\\", "\\\\").replace(",", "\\,")
        started = time.monotonic()
        try:
            code = subprocess.run([program, f"--source-file=*{name}", f"-tc={pattern}"],
                                  capture_output=True, text=True, cwd=work,
                                  timeout=180).returncode
        except subprocess.TimeoutExpired:
            code = "TIMED OUT"
        took = time.monotonic() - started
        time.sleep(2)
        left = compilers()
        mark = " CVMCompiler LEFT RUNNING: " + str(left) if left else ""
        print(f"{took:7.1f} s  exit {code}  {name}: {case}{mark}", flush=True)
        for row in left:
            subprocess.run(["kill", "-9", row[0]])
