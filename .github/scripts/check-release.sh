#!/usr/bin/env bash
# Unpacks the macOS release archive somewhere new and uses it as a person with no checkout would
# (docs/getting-started.md): the tools find the installed engine, build a game's C++ with the
# compiler on PATH against the installed gameplay SDK and load it, and export the game, fetching
# the engine's dependencies into the per-machine cache.
#
#   check-release.sh <slashengine-macos.tar.gz> <checkout>
set -euo pipefail
archive=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
checkout=$(cd "$2" && pwd)
work=$(mktemp -d)
tar -C "$work" -xzf "$archive"
root=$work/slashengine
cli=$root/bin/slash-cli

# The tools link only the system's libraries, so they run on any Mac.
for tool in slash-cli slash-mcp slash-game; do
  if otool -L "$root/bin/$tool" | tail -n +2 | grep -v -E '^[[:space:]]*(/usr/lib/|/System/)'; then
    echo "$tool links a library outside the system" >&2
    exit 1
  fi
done
"$cli" version

# Nothing installed names the build it came from.
sdk=$root/share/slashengine/gameplay-sdk.cmake
cat "$sdk"
if grep -q "$checkout" "$sdk"; then
  echo "the installed gameplay SDK names the checkout" >&2
  exit 1
fi

# The checkout moves away, so the tools can only use what was installed.
mv "$checkout" "$checkout.moved"
trap 'mv "$checkout.moved" "$checkout"' EXIT

game=$work/game
"$cli" init "$game" > /dev/null
"$cli" build --project "$game" | tee "$work/build.json"
jq -e '.components == ["Spin"]' "$work/build.json"
grep -q "$sdk" "$game/.slash/gameplay/build.log"
"$cli" call engine.describe --project "$game" | jq -e '.components | map(.name) | index("Spin")'

# A debug build: one architecture, unoptimized, so the whole engine compiles quickly.
env -u CPM_SOURCE_CACHE "$cli" export macos --debug --project "$game" --test > "$work/export.json" \
  || { cat "$work/export.json"; tail -n 50 "$game/.slash/export/macos/export.log"; exit 1; }
cat "$work/export.json"
jq -e '.test.passed' "$work/export.json"
test -d "$HOME/.slash/cpm-cache"
