#!/bin/bash
# The background half of session-start.sh, run from the project folder under its lock: configure
# the dev preset to compile through ccache, build slash-cli, install the web toolchain, build the
# other tools and the unit tests, then install the Android SDK where the network reaches Google's. Writes running, done or failed to .slash/session-setup.status.
set -uo pipefail

cli=./build/dev/tools/slash-cli/slash-cli
status=.slash/session-setup.status

step() {
    echo "== $1 ($(date -u +%H:%M:%S))"
    shift
    if ! "$@"; then
        echo "== failed: $*"
        echo failed >"$status"
        exit 1
    fi
}

# All in a function, so bash has read the whole file before running any of it: a checkout that
# changes this file while it runs can't change what it does.
main() {
    echo running >"$status"

    # ccache on the command line rather than in the environment, so the builds that exports and
    # build.check run with their own compilers are left alone. ccache can't keep compiles that use
    # precompiled headers (SDL's and Jolt's), so those are off.
    # Debug links of slash_tests and the tools take GBs each, so a 16 GB machine links one at a
    # time.
    configure=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
        -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON -DCMAKE_JOB_POOLS=link=1 -DCMAKE_JOB_POOL_LINK=link)

    # Wayland is pinned from gitlab.freedesktop.org, which cloud sessions' default network blocks.
    # Ubuntu 20.04's source package is the same 1.18.0 release (its COPYING has the hash
    # dependencies.json pins), so a first configure takes it from Ubuntu's archive instead.
    wayland_tag=$(jq -r .dependencies.wayland.tag cmake/dependencies.json)
    wayland_dir=$PWD/.cpm-cache/wayland-ubuntu-$wayland_tag
    if [ ! -f build/dev/CMakeCache.txt ] && [ "$wayland_tag" = 1.18.0 ] &&
        ! timeout 30 git ls-remote --exit-code https://gitlab.freedesktop.org/wayland/wayland.git \
            "$wayland_tag" >/dev/null 2>&1; then
        if [ ! -f "$wayland_dir/COPYING" ]; then
            echo "== fetch Wayland $wayland_tag from Ubuntu's archive ($(date -u +%H:%M:%S))"
            archive=$(mktemp)
            if curl -sSfL -o "$archive" \
                http://archive.ubuntu.com/ubuntu/pool/main/w/wayland/wayland_1.18.0.orig.tar.gz &&
                echo "102d5556db1524a7737ab76e358b03e5b92a7718f1ac1520bf9686bfd09c8e4a  $archive" |
                sha256sum --check --quiet; then
                mkdir -p "$wayland_dir"
                tar -xzf "$archive" -C "$wayland_dir" --strip-components=1
            fi
            rm -f "$archive"
        fi
        if [ -f "$wayland_dir/COPYING" ]; then
            configure+=("-DCPM_wayland_SOURCE=$wayland_dir")
        fi
    fi

    step "configure the dev preset" cmake --preset dev "${configure[@]}"
    step "build slash-cli" cmake --build --preset dev --target slash-cli
    step "install the web toolchain" "$cli" toolchain install web
    # Not the examples: in a Debug build each links the whole engine with its debug info, some
    # 20 GB in all, more than a session's disk can spare. Build the ones a task needs.
    step "build the tools and tests" cmake --build --preset dev \
        --target slash-cli slash-mcp slash-game slash_tests
    # The Android SDK, NDK and JDK, accepting Google's Android SDK license for this project's
    # sessions as its owner asked. Last and never fatal: dl.google.com is blocked on some networks,
    # cloud sessions' default one included, and nothing but Android exports needs it.
    if timeout 20 curl -sSfI -o /dev/null https://dl.google.com/android/repository/repository2-1.xml; then
        echo "== install the Android SDK ($(date -u +%H:%M:%S))"
        if ! "$cli" toolchain install android --accept-android-sdk-license; then
            echo "== skipped the Android SDK: its install failed"
        fi
    else
        echo "== skipped the Android SDK: dl.google.com is unreachable from this network"
    fi
    echo "== done ($(date -u +%H:%M:%S))"
    echo done >"$status"
}

main
exit
