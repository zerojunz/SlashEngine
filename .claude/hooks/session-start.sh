#!/bin/bash
# Sets up a Claude Code cloud session so it can build, test, lint and syntax-check for the web
# without working out the steps first. Runs only in cloud sessions (CLAUDE_CODE_REMOTE).
#
# In the foreground (a minute or so): the Linux packages CI's Linux jobs install, ccache, and
# clang-format and clang-tidy 18 as CI's lint job uses them. Then session-build.sh, in the
# background, holding /tmp/slash-session-setup.lock: configure the dev preset to compile through
# ccache (fetching the dependencies), build slash-cli, install the web toolchain (Emscripten, for
# the libc++ portability check in AGENTS.md), build the other tools and the unit tests, then install
# the Android SDK (accepting Google's license) when dl.google.com answers, and skip it when not.
# Wayland, pinned from gitlab.freedesktop.org, comes from Ubuntu's archive where that is blocked.
#
# Safe to run again: installed packages, the configured build and installed toolchains are kept.
set -euo pipefail

if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
    exit 0
fi

project=${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/../.." && pwd)}
lock=/tmp/slash-session-setup.lock
log=$project/.slash/session-setup.log

# Keep in step with LINUX_SDL_DEPS and the Linux test packages in .github/workflows/ci.yml.
packages=(ninja-build libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev
    libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev libegl-dev libgl-dev libdrm-dev
    libgbm-dev libasound2-dev libpulse-dev libudev-dev libdbus-1-dev
    mesa-vulkan-drivers libgl1 libgles2 libegl-mesa0 libgl1-mesa-dri at-spi2-core
    ccache clang-format-18 clang-tidy-18 pkgconf jq)

sudo=
if [ "$(id -u)" != 0 ]; then
    sudo=sudo
fi
missing=()
for package in "${packages[@]}"; do
    if ! dpkg-query -W -f '${Status}' "$package" 2>/dev/null | grep -q "install ok installed"; then
        missing+=("$package")
    fi
done
if [ ${#missing[@]} -gt 0 ]; then
    $sudo apt-get update -qq >/dev/null
    DEBIAN_FRONTEND=noninteractive $sudo apt-get install -y -qq "${missing[@]}" >/dev/null
fi

ccache --max-size=10G >/dev/null

mkdir -p "$project/.slash"
cd "$project"

# The rest takes half an hour on four cores, so it runs on after the session starts.
setsid nohup flock "$lock" "$project/.claude/hooks/session-build.sh" >"$log" 2>&1 </dev/null &

# A SessionStart hook's output goes to the session as context.
cat <<EOF
SlashEngine session setup: the Linux packages, ccache and clang-format/clang-tidy 18 are installed.
The dev build and the web toolchain are being set up in the background (log: .slash/session-setup.log,
state: .slash/session-setup.status). Before building, wait for it: flock $lock true
The Android SDK installs last when dl.google.com is reachable; the log says if it was skipped.
EOF
