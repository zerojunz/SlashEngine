#!/usr/bin/env bash
# Unpacks a release archive somewhere new and uses it as a person with no checkout would
# (docs/getting-started.md): the tools find the installed engine, build a game's C++ with the
# compiler on PATH against the installed gameplay SDK and load it, and export the game for the
# machine it runs on, fetching the engine's dependencies into the per-machine cache. On Windows it
# runs in Git Bash after MSVC's environment is set up, so dumpbin and the compiler are on PATH.
#
#   check-release.sh <slashengine-macos.tar.gz | slashengine-linux-<x86_64|arm64>.tar.gz |
#                     slashengine-windows-x64.zip> <checkout>
set -euo pipefail
archive=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
checkout=$(cd "$2" && pwd)
# The same folder as Windows programs write it (D:/a/...), which the SDK file would hold.
checkout_mixed=$(command -v cygpath > /dev/null && cygpath -m "$checkout" || true)
work=$(mktemp -d)
(cd "$work" && cmake -E tar xf "$archive")
root=$work/slashengine
cli=$root/bin/slash-cli

# The tools link only the system's libraries, so they run with no other install: on a Mac those
# under /usr/lib and /System, on Linux the C and C++ runtimes (glibc before 2.34 keeps the
# pseudo-terminal calls in libutil, and later ones still ship it; SDL loads the display and sound
# libraries it finds when it starts), on Windows those in System32, Visual C++'s runtime among
# them where Visual Studio is installed.
case "$(uname -s)" in
  Darwin)
    platform=macos
    libraries() { otool -L "$1" | tail -n +2 | grep -v -E '^[[:space:]]*(/usr/lib/|/System/)' || true; }
    ;;
  Linux)
    platform=linux
    libraries() {
      ldd "$1" | grep -v -E '^[[:space:]]*(linux-vdso|/lib(64)?/ld-linux)|(libc|libm|libdl|libpthread|librt|libutil|libstdc\+\+|libgcc_s)\.so' || true
    }
    ;;
  MINGW*|MSYS*)
    platform=windows
    system32=$(cygpath -u "$SYSTEMROOT")/System32
    libraries() {
      dumpbin -nologo -dependents "$1.exe" | tr -d '\r' | sed -n 's/^    \([^ ]*\.[dD][lL][lL]\)$/\1/p' \
        | while read -r dll; do
            case "$dll" in api-ms-win-*) continue ;; esac
            [ -f "$system32/$dll" ] || echo "$dll"
          done
    }
    ;;
  *) echo "no release for $(uname -s)" >&2; exit 1 ;;
esac
for tool in slash-cli slash-mcp slash-game; do
  if [ -n "$(libraries "$root/bin/$tool")" ]; then
    libraries "$root/bin/$tool"
    echo "$tool links a library outside the system" >&2
    exit 1
  fi
done
"$cli" version

# Nothing installed names the build it came from.
sdk=$root/share/slashengine/gameplay-sdk.cmake
cat "$sdk"
if grep -q -F -e "$checkout" ${checkout_mixed:+-e "$checkout_mixed"} "$sdk"; then
  echo "the installed gameplay SDK names the checkout" >&2
  exit 1
fi

# The checkout moves away, so the tools can only use what was installed. On Windows, MSVC's PDB
# server and telemetry outlive the build in the checkout, and Windows won't move a folder a
# running program is in, so they stop first.
if [ "$platform" = windows ]; then
  taskkill //F //IM mspdbsrv.exe > /dev/null 2>&1 || true
  taskkill //F //IM vctip.exe > /dev/null 2>&1 || true
fi
for try in 1 2 3 4 5; do
  mv "$checkout" "$checkout.moved" && break
  if [ "$try" = 5 ]; then
    # Which programs are still running, to find the one holding the checkout.
    if [ "$platform" = windows ]; then
      powershell -NoProfile -Command 'Get-CimInstance Win32_Process |
        Format-Table -AutoSize -Wrap ProcessId, ParentProcessId, Name, CommandLine | Out-String -Width 300'
    fi
    exit 1
  fi
  sleep 2
done
trap 'mv "$checkout.moved" "$checkout"' EXIT

game=$work/game
"$cli" init "$game" > /dev/null
"$cli" build --project "$game" | tee "$work/build.json"
jq -e '.components == ["Spin"]' "$work/build.json"
if [ "$platform" = windows ]; then
  # Windows programs write the path their own way; the installed SDK's place is enough.
  grep -q -E 'share[/\\]slashengine[/\\]gameplay-sdk\.cmake' "$game/.slash/gameplay/build.log"
else
  grep -q "$sdk" "$game/.slash/gameplay/build.log"
fi
"$cli" call engine.describe --project "$game" | jq -e '.components | map(.name) | index("Spin")'

# Linux games build for x86_64 unless a preset says otherwise, so on an arm64 machine the linux
# preset builds for arm64, to run the game there.
if [ "$platform" = linux ] && [ "$(uname -m)" = aarch64 ]; then
  echo '{"format": 1, "presets": {"linux": {"platform": "linux", "linux": {"arch": "arm64"}}}}' \
    > "$game/export_presets.json"
fi
# A debug build: one architecture, unoptimized, so the whole engine compiles quickly.
env -u CPM_SOURCE_CACHE "$cli" export "$platform" --debug --project "$game" --test \
  > "$work/export.json" \
  || { cat "$work/export.json"; tail -n 50 "$game/.slash/export/$platform/export.log"; exit 1; }
cat "$work/export.json"
jq -e '.test.passed' "$work/export.json"
test -d "$HOME/.slash/cpm-cache"
