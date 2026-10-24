#!/usr/bin/env python3
"""Runs clang-tidy on files, skipping those it passed before with exactly the same input.

clang-tidy takes over an hour on every file of the engine, but most pull requests change few
translation units. A file's result depends only on clang-tidy, its configuration, the compile
flags and the preprocessed source (comments kept, so NOLINT counts), so this hashes those and
records the hashes of files that passed in a cache folder that CI keeps between runs. A file
whose hash is there is skipped; any other runs as usual, and its findings fail the run.

    clang-tidy-cached.py --cache DIR -p BUILD [--clang-tidy clang-tidy-18] file.cpp ...

Each hash file used is touched, so a job can drop the ones it didn't use afterwards.
"""

import argparse
import hashlib
import json
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def compile_arguments(entry):
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry["command"])


def preprocess_arguments(arguments):
    """The compile command, writing the preprocessed source with comments to stdout."""
    result = []
    skip = False
    for arg in arguments:
        if skip:
            skip = False
            continue
        if arg in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
            continue
        if arg in ("-c", "-MD", "-MMD") or arg.startswith(("-o", "-MF")):
            continue
        result.append(arg)
    return result + ["-E", "-C", "-P", "-o", "-"]


def file_key(entry, tool_identity):
    arguments = compile_arguments(entry)
    preprocessed = subprocess.run(preprocess_arguments(arguments), cwd=entry["directory"],
                                  capture_output=True, check=False)
    if preprocessed.returncode != 0:
        return None
    digest = hashlib.sha256()
    digest.update(tool_identity)
    digest.update("\0".join(a for a in arguments if not a.startswith("-o")).encode())
    digest.update(preprocessed.stdout)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cache", required=True, type=Path)
    parser.add_argument("-p", dest="build", required=True, type=Path)
    parser.add_argument("--clang-tidy", default="clang-tidy")
    parser.add_argument("files", nargs="+")
    options = parser.parse_args()

    entries = {}
    for entry in json.loads((options.build / "compile_commands.json").read_text()):
        entries[Path(entry["directory"], entry["file"]).resolve()] = entry

    version = subprocess.run([options.clang_tidy, "--version"], capture_output=True,
                             check=True).stdout
    tool_identity = version
    for config in (".clang-tidy", ".clang-format"):
        tool_identity += (ROOT / config).read_bytes()
    options.cache.mkdir(parents=True, exist_ok=True)

    failed = False
    for name in options.files:
        entry = entries.get(Path(name).resolve())
        key = file_key(entry, tool_identity) if entry else None
        record = options.cache / key if key else None
        if record is not None and record.exists():
            record.touch()
            continue
        result = subprocess.run([options.clang_tidy, "-p", str(options.build), "--quiet", name],
                                check=False)
        if result.returncode != 0:
            failed = True
        elif record is not None:
            record.touch()
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
