#!/usr/bin/env bash
# check-owner-name.sh: fail if the owner's name appears in the repo or in a pull request.
#
# Rule (the owner, 20 Nov 2026): never write the owner's name (first name, full name, username, or a
# possessive of them) in code, comments, tests, docs, commit messages, PR titles, PR bodies or PR comments.
# Write "the owner" instead; test data uses a neutral name such as "Player".
#
# Usage:
#   scripts/check-owner-name.sh                 check tracked files (git grep, whole words, case-sensitive)
#   scripts/check-owner-name.sh --pr            also check PR_TITLE, PR_BODY and the commit messages in
#                                               BASE_SHA..HEAD_SHA (set by CI from the pull_request event)
#   scripts/check-owner-name.sh --text FILE     check one text file (e.g. a PR body draft) and nothing else
# Exempt: third-party licence texts and author lists (other people who share the name). Extra exempt
# pathspecs, one per line, go in .owner-name-exempt at the repo root.
# The pattern is assembled from pieces so that this file never contains the name itself.
set -uo pipefail

first="D""an"; full="D""aniel"; user="d""aniellee"
PAT="${first}|${full} ?Lee|${full}Lee|${user}"

exempt=(':(exclude)**/licenses/**' ':(exclude)**/AUTHORS' ':(exclude)**/AUTHORS.*' ':(exclude)**/COPYING*'
        ':(exclude)**/Copyright' ':(exclude)**/LICENSE' ':(exclude)**/LICENSE.*' ':(exclude)**/THIRD_PARTY*'
        ':(exclude)**/third_party/**' ':(exclude)**/vendor/**')
root=$(git rev-parse --show-toplevel 2>/dev/null) || { echo "check-owner-name: not in a git repo" >&2; exit 2; }
if [ -f "$root/.owner-name-exempt" ]; then
  while IFS= read -r p; do [ -n "$p" ] && [ "${p#\#}" = "$p" ] && exempt+=(":(exclude)$p"); done < "$root/.owner-name-exempt"
fi

fail=0
scan_text() {   # $1 label, stdin text
  local hits
  hits=$(grep -n -w -E "$PAT" || true)
  if [ -n "$hits" ]; then
    echo "check-owner-name: the owner's name appears in $1:"; echo "$hits" | sed 's/^/  /' | head -20
    fail=1
  fi
}

if [ "${1:-}" = "--text" ]; then
  [ -f "${2:-}" ] || { echo "usage: $0 --text FILE" >&2; exit 2; }
  scan_text "$2" < "$2"
else
  hits=$(cd "$root" && git grep -I -n -w -E "$PAT" -- . "${exempt[@]}" || true)
  if [ -n "$hits" ]; then
    echo "check-owner-name: the owner's name appears in tracked files (write \"the owner\" instead):"
    echo "$hits" | sed 's/^/  /' | head -50
    [ "$(echo "$hits" | wc -l)" -gt 50 ] && echo "  ... $(echo "$hits" | wc -l) lines in all"
    fail=1
  fi
  if [ "${1:-}" = "--pr" ]; then
    printf '%s\n' "${PR_TITLE:-}" | scan_text "the PR title"
    printf '%s\n' "${PR_BODY:-}" | scan_text "the PR body"
    if [ -n "${BASE_SHA:-}" ] && [ -n "${HEAD_SHA:-}" ]; then
      git -C "$root" log --format='%H %B' "$BASE_SHA..$HEAD_SHA" 2>/dev/null | scan_text "commit messages ($BASE_SHA..$HEAD_SHA)"
    fi
  fi
fi

if [ $fail -eq 0 ]; then echo "check-owner-name: OK"; fi
exit $fail
