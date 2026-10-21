#!/usr/bin/env bash
# Checks a Linux export: it needs nothing newer than glibc 2.31 and GCC 10's libstdc++, and it
# runs headless on Ubuntu 20.04, the oldest Linux games support. An x86_64 game also opens its
# window there on an X server and a Wayland compositor: X11 first, Wayland when there is no X
# server or the project asks for it first.
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

if [ "$arch" = amd64 ]; then
  docker run --rm -v "$folder:/game:ro" -e GAME="$(basename "$exe")" ubuntu:20.04 \
    bash -euo pipefail -c '
      export DEBIAN_FRONTEND=noninteractive
      apt-get update -qq
      apt-get install -y -qq --no-install-recommends weston xvfb libwayland-egl1 > /dev/null
      export XDG_RUNTIME_DIR=/tmp/xdg
      mkdir -m 700 -p "$XDG_RUNTIME_DIR"
      weston --backend=headless-backend.so --socket=wayland-1 --idle-time=0 > /tmp/weston.log 2>&1 &
      Xvfb :1 -screen 0 1280x720x24 > /tmp/xvfb.log 2>&1 &
      for _ in $(seq 100); do
        test -S "$XDG_RUNTIME_DIR/wayland-1" && test -S /tmp/.X11-unix/X1 && break
        sleep 0.1
      done
      if ! test -S "$XDG_RUNTIME_DIR/wayland-1" || ! test -S /tmp/.X11-unix/X1; then
        echo "Weston or Xvfb did not start" >&2
        tail -n 20 /tmp/weston.log /tmp/xvfb.log >&2
        exit 1
      fi
      cp -r /game /tmp/game
      cd /tmp/game
      # A failure shows the game, then the symbols SDL looked up most often without finding them:
      # it tries each in every X11 or Wayland library it loads, so those missing from all of them
      # come first, and one SDL needs that this system lacks is the likely cause.
      driver() {
        output=$(env "$@" "./$GAME" --ticks 30 2>&1) || {
          echo "$output" >&2
          env "$@" LD_DEBUG=libs "./$GAME" --ticks 1 2>&1 |
            sed -nE "s/.*undefined symbol: _?([A-Za-z0-9_]+).*/missing \1/p" |
            sort | uniq -c | sort -rn | head -n 5 >&2 || true
          return 1
        }
        grep -o "\"video_driver\":\"[a-z0-9]*\"" <<< "$output" | cut -d\" -f4
      }
      test "$(driver DISPLAY=:1 WAYLAND_DISPLAY=wayland-1)" = x11
      test "$(driver WAYLAND_DISPLAY=wayland-1)" = wayland
      sed -i "s/\"format\": 1,/\"format\": 1, \"window\": {\"linux_display_driver\": \"wayland\"},/" \
        data/project.json
      test "$(driver DISPLAY=:1 WAYLAND_DISPLAY=wayland-1)" = wayland
      test "$(driver DISPLAY=:1 WAYLAND_DISPLAY=none)" = x11
      echo "windowed: X11 first, Wayland without an X server or when the project asks"
    '
fi
