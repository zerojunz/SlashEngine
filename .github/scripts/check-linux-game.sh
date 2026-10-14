#!/usr/bin/env bash
# Checks a Linux export: it needs nothing newer than glibc 2.31 and GCC 10's libstdc++, and it
# runs headless on Ubuntu 20.04, the oldest Linux games support.
# Usage: check-linux-game.sh <export folder> <amd64|arm64>
set -euo pipefail
folder=$(cd "$1" && pwd)
arch=$2
exe=$(find "$folder" -maxdepth 1 -type f -perm -u+x | head -n 1)
test -n "$exe"

newest() { readelf -V --wide "$exe" | grep -o "$1_[0-9][0-9.]*" | sed "s/^$1_//" | sort -uV | tail -n 1; }
at_most() { test "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -n 1)" = "$2"; }
glibc=$(newest GLIBC)
glibcxx=$(newest GLIBCXX)
echo "$(basename "$exe") needs glibc $glibc and libstdc++ GLIBCXX_$glibcxx"
at_most "$glibc" 2.31
at_most "$glibcxx" 3.4.28

machine=$(readelf -h "$exe" | sed -n 's/^ *Machine: *//p')
echo "machine: $machine"
case "$arch" in
  amd64) grep -q "X86-64" <<< "$machine" ;;
  arm64) grep -q "AArch64" <<< "$machine" ;;
  *) echo "unknown arch $arch"; exit 2 ;;
esac

output=$(docker run --rm --platform "linux/$arch" -v "$folder:/game" -w /game ubuntu:20.04 \
  "/game/$(basename "$exe")" --headless --ticks 60)
echo "$output"
grep -q '"ticks":60' <<< "$output"
