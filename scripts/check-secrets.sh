#!/usr/bin/env bash
# check-secrets.sh: fail if a credential is committed: an API key, token, password, private key,
# signing certificate or store login, or a file that holds one.
#
# Rule (the owner, 27 Nov 2026): the owner's own credentials, and every developer's, never reach
# the repository. They live in environment variables only: GitHub Actions secrets in CI (APPLE_*,
# BENCH_*), the developer's own shell for exports and store publishing (BUTLER_API_KEY,
# STEAM_USERNAME, EPIC_CLIENT_SECRET, GOG_PASSWORD, SLASH_*_PASSWORD, ...). Scripts read them from
# there and never print them.
#
# It also fails a commit whose author or committer e-mail is a machine's own (login@host.local),
# which carries the login and computer name.
#
# Usage:
#   scripts/check-secrets.sh            check tracked file names, and the commits in origin/main..HEAD
#   scripts/check-secrets.sh --range    check the commits in BASE_SHA..HEAD_SHA (set by CI)
#   scripts/check-secrets.sh --all      check every commit in the history (slow)
# Commit contents go through gitleaks (MIT) with .gitleaks.toml: its own rules plus this project's
# credential variables. gitleaks runs from PATH (brew install gitleaks), else the pinned release
# below is downloaded and its SHA-256 checked (Linux and macOS). Findings print redacted, so the
# log never carries the secret on. A false positive goes in .gitleaks.toml's allowlist, or its line
# gets a "gitleaks:allow" comment.
#
# If this fails on a real secret: rotate it first (removing the commit doesn't un-leak it), then
# drop it from the branch before it merges.
set -uo pipefail

gitleaks_version=8.24.3
sha256_for() {   # the release archive's SHA-256, from its checksums.txt
  case "$1" in
    linux_x64) echo 9991e0b2903da4c8f6122b5c3186448b927a5da4deef1fe45271c3793f4ee29c ;;
    linux_arm64) echo 5f2edbe1f49f7b920f9e06e90759947d3c5dfc16f752fb93aaafc17e9d14cf07 ;;
    darwin_x64) echo 41c44ae8ad1d6eef57d4526ad0fd67d8129eee9a856f55c2b3b9395fd3d9ec0f ;;
    darwin_arm64) echo b90f13bb8c90ab72083d9b0c842e39dafb82c0e5c3f872f407366b7a58909013 ;;
  esac
}

# Files that hold credentials, by name; .gitignore keeps them out of `git add .` too.
files_pat='(^|/)(\.env(\.[^/]*)?|[^/]*\.(p12|pfx|p8|pem|key|keystore|jks|mobileprovision|provisionprofile|cer|certSigningRequest|keychain|keychain-db)|id_(rsa|dsa|ecdsa|ed25519)(\.pub)?|\.netrc|\.npmrc|\.pypirc|credentials\.json|service-account[^/]*\.json|google-services\.json|GoogleService-Info\.plist|config\.vdf|ssfn[^/]*|butler_creds)$'
files_ok='(^|/)\.env\.example$'

root=$(git rev-parse --show-toplevel 2>/dev/null) || { echo "check-secrets: not in a git repo" >&2; exit 2; }
cd "$root" || exit 2

fail=0
hits=$(git ls-files | grep -E "$files_pat" | grep -v -E "$files_ok" || true)
if [ -n "$hits" ]; then
  echo "check-secrets: tracked files that hold credentials (keep them out of the repo; read the secret from an environment variable):"
  echo "$hits" | sed 's/^/  /'
  fail=1
fi

case "${1:-}" in
  --range)
    base=${BASE_SHA:-}; head=${HEAD_SHA:-HEAD}
    # A push of a new ref, a tag or a forced push has no usable base: check the last commit.
    if [ -z "$base" ] || [ -z "${base//0/}" ] || ! git cat-file -e "$base^{commit}" 2>/dev/null; then
      base="$head~1"
    fi
    log_opts="$base..$head" ;;
  --all) log_opts="--all" ;;
  "")
    if git rev-parse -q --verify origin/main > /dev/null; then log_opts="origin/main..HEAD"; else log_opts="HEAD~1..HEAD"; fi ;;
  *) echo "usage: $0 [--range|--all]" >&2; exit 2 ;;
esac

# A commit made on a machine with no user.email set records the login and host name
# (name@Someones-MacBook.local): personal details, not a credential, but they don't belong either.
if [ "$log_opts" != "--all" ]; then
  local_mail='@(.*[.](local|lan|localdomain|home|internal)|[(]none[)]|localhost)$'
  hosts=$(git log --format='%h %ae %ce' "$log_opts" 2>/dev/null \
    | awk -v re="$local_mail" 'BEGIN { IGNORECASE = 1 } tolower($2) ~ re || tolower($3) ~ re { print $1 }')
  if [ -n "$hosts" ]; then
    echo "check-secrets: these commits carry a machine's login and host name as their e-mail (not shown here);"
    echo "  set a noreply identity (git config --global user.email <id>+<login>@users.noreply.github.com)"
    echo "  and recommit them: $(echo $hosts)"
    fail=1
  fi
fi

gitleaks=$(command -v gitleaks || true)
if [ -z "$gitleaks" ]; then
  case "$(uname -s)_$(uname -m)" in
    Linux_x86_64) platform=linux_x64 ;;
    Linux_aarch64 | Linux_arm64) platform=linux_arm64 ;;
    Darwin_x86_64) platform=darwin_x64 ;;
    Darwin_arm64) platform=darwin_arm64 ;;
    *) echo "check-secrets: install gitleaks ${gitleaks_version} (brew install gitleaks)" >&2; exit 2 ;;
  esac
  tools=$(mktemp -d); trap 'rm -rf "$tools"' EXIT
  archive="$tools/gitleaks.tar.gz"
  url="https://github.com/gitleaks/gitleaks/releases/download/v${gitleaks_version}/gitleaks_${gitleaks_version}_${platform}.tar.gz"
  curl -fsSL --retry 3 -o "$archive" "$url" || { echo "check-secrets: couldn't download $url" >&2; exit 2; }
  if command -v sha256sum > /dev/null; then sum=$(sha256sum "$archive"); else sum=$(shasum -a 256 "$archive"); fi
  if [ "${sum%% *}" != "$(sha256_for "$platform")" ]; then
    echo "check-secrets: gitleaks ${gitleaks_version} download doesn't match its SHA-256" >&2; exit 2
  fi
  tar -xzf "$archive" -C "$tools" gitleaks
  gitleaks="$tools/gitleaks"
fi

if [ "$log_opts" = "--all" ] || [ -n "$(git rev-list "$log_opts" 2>/dev/null | head -n 1)" ]; then
  "$gitleaks" git --no-banner --redact --exit-code 3 --config .gitleaks.toml \
    --log-opts="$log_opts" --log-level warn .
  status=$?
  if [ $status -eq 3 ]; then
    echo "check-secrets: gitleaks found a credential in $log_opts (rotate it, then drop it from the branch)"
    fail=1
  elif [ $status -ne 0 ]; then
    echo "check-secrets: gitleaks couldn't run (exit $status)" >&2
    exit 2
  fi
fi

if [ $fail -eq 0 ]; then echo "check-secrets: OK"; fi
exit $fail
