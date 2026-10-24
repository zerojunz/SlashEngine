#!/usr/bin/env python3
"""Syntax-checks C++ files against libc++ with Emscripten's Clang, before CI does.

GCC's libstdc++ accepts code that libc++ (the web, Android and llvm-mingw builds) and MSVC's STL
refuse: their std::array, std::string_view and std::span iterators are classes, not pointers, and
they include fewer headers from each other. A Linux build alone misses that. This compiles the
given files, or the .cpp files changed since origin/main, with the flags the dev build's
compile_commands.json has for them, against Emscripten's libc++, and prints only errors.

    .github/scripts/check-libcxx.py [--build build/dev] [file.cpp ...]

Needs the web toolchain (slash-cli toolchain install web) and a configured build.
"""

import argparse
import json
import os
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def toolchains_dir():
    if os.environ.get("SLASH_TOOLCHAINS_DIR"):
        return Path(os.environ["SLASH_TOOLCHAINS_DIR"])
    return Path.home() / ".slash" / "toolchains"


def find_emscripten():
    found = sorted(toolchains_dir().glob("emscripten-*"))
    if not found:
        sys.exit("no Emscripten in %s; run: slash-cli toolchain install web" % toolchains_dir())
    return found[-1]


def sysroot(emscripten):
    """Emscripten fills its sysroot's headers the first time it compiles anything."""
    root = emscripten / "emscripten" / "cache" / "sysroot"
    if (root / "include" / "c++" / "v1").is_dir():
        return root
    node = sorted(toolchains_dir().glob("node-*"))
    if not node:
        sys.exit("no Node.js in %s; run: slash-cli toolchain install web" % toolchains_dir())
    config = emscripten / "slash-check.config"
    config.write_text("LLVM_ROOT = '%s/bin'\nBINARYEN_ROOT = '%s'\nNODE_JS = '%s/bin/node'\n"
                      % (emscripten, emscripten, node[-1]))
    empty = emscripten / "slash-check-empty.c"
    empty.write_text("")
    subprocess.run([sys.executable, str(emscripten / "emscripten" / "emcc.py"), "-c",
                    str(empty), "-o", os.devnull],
                   env=dict(os.environ, EM_CONFIG=str(config)), check=True)
    return root


def changed_files():
    base = subprocess.run(["git", "merge-base", "origin/main", "HEAD"], cwd=ROOT,
                          capture_output=True, text=True, check=True).stdout.strip()
    names = subprocess.run(["git", "diff", "--name-only", "--diff-filter=d", base], cwd=ROOT,
                           capture_output=True, text=True, check=True).stdout.split()
    names += subprocess.run(["git", "ls-files", "--others", "--exclude-standard"], cwd=ROOT,
                            capture_output=True, text=True, check=True).stdout.split()
    return sorted({n for n in names if n.endswith(".cpp")})


# Flags from the GCC command that mean the same to Clang and don't name the host.
KEPT_PREFIXES = ("-I", "-D", "-U", "-std=", "-isystem", "-include", "-fno-exceptions",
                 "-fexceptions", "-fno-rtti", "-frtti")


def portable_flags(command):
    args = shlex.split(command) if isinstance(command, str) else list(command)
    kept = []
    i = 1
    while i < len(args):
        arg = args[i]
        if arg in ("-isystem", "-include", "-I", "-D") and i + 1 < len(args):
            kept += [arg, args[i + 1]]
            i += 2
            continue
        if arg.startswith(KEPT_PREFIXES):
            kept.append(arg)
        i += 1
    return kept


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", default="build/dev", help="build folder (default build/dev)")
    parser.add_argument("files", nargs="*", help="files to check (default: changed .cpp files)")
    options = parser.parse_args()

    database = ROOT / options.build / "compile_commands.json"
    if not database.is_file():
        sys.exit("no %s; configure the build first (cmake --preset dev)" % database)
    commands = {}
    for entry in json.loads(database.read_text()):
        path = Path(entry["directory"], entry["file"]).resolve()
        commands.setdefault(path, entry)

    files = options.files or changed_files()
    if not files:
        print("no changed .cpp files")
        return 0

    emscripten = find_emscripten()
    clang = emscripten / "bin" / "clang++"
    root = sysroot(emscripten)
    failed = 0
    for name in files:
        path = (ROOT / name).resolve() if not Path(name).is_absolute() else Path(name)
        entry = commands.get(path)
        if entry is None:
            print("skipped %s: not in %s" % (name, database))
            continue
        flags = portable_flags(entry.get("command") or entry["arguments"])
        result = subprocess.run(
            [str(clang), "--target=wasm32-unknown-emscripten", "--sysroot=%s" % root,
             "-Xclang", "-iwithsysroot/include/compat", "-fsyntax-only", "-w"] + flags
            + [str(path)], cwd=entry["directory"], capture_output=True, text=True)
        if result.returncode != 0:
            failed += 1
            print(result.stderr, end="")
    print("%d of %d files failed against libc++" % (failed, len(files)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
