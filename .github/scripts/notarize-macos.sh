#!/usr/bin/env bash
# Signs the installed macOS tools with a Developer ID and the hardened runtime, then has Apple
# notarize them, so a downloaded copy opens without a warning. The release job of
# .github/workflows/ci.yml runs it on a version tag when the repository has these secrets:
#
#   APPLE_DEVELOPER_ID_CERTIFICATE           the Developer ID Application certificate and its key,
#                                            exported from Keychain Access as .p12, in base64
#   APPLE_DEVELOPER_ID_CERTIFICATE_PASSWORD  the password the .p12 was exported with
#   APPLE_API_KEY                            an App Store Connect API key (.p8), in base64
#   APPLE_API_KEY_ID, APPLE_API_ISSUER       that key's ID and its issuer ID
#
#   notarize-macos.sh <install prefix>
#
# Bare executables can't carry a stapled ticket, so Gatekeeper looks the notarization up online
# the first time a tool runs.
set -euo pipefail
prefix=$(cd "$1" && pwd)
for name in APPLE_DEVELOPER_ID_CERTIFICATE APPLE_DEVELOPER_ID_CERTIFICATE_PASSWORD APPLE_API_KEY \
            APPLE_API_KEY_ID APPLE_API_ISSUER; do
  if [ -z "${!name:-}" ]; then
    echo "$name isn't set" >&2
    exit 2
  fi
done
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# A keychain of its own, which the runner throws away, holding only the certificate.
keychain=$work/release.keychain-db
keychain_password=$(uuidgen)
security create-keychain -p "$keychain_password" "$keychain"
security set-keychain-settings -lut 3600 "$keychain"
security unlock-keychain -p "$keychain_password" "$keychain"
printf '%s' "$APPLE_DEVELOPER_ID_CERTIFICATE" | base64 --decode > "$work/developer-id.p12"
security import "$work/developer-id.p12" -k "$keychain" \
  -P "$APPLE_DEVELOPER_ID_CERTIFICATE_PASSWORD" -T /usr/bin/codesign
security set-key-partition-list -S apple-tool:,apple: -s -k "$keychain_password" "$keychain" \
  > /dev/null
# shellcheck disable=SC2046 # the keychains listed are separate words
security list-keychains -d user -s "$keychain" $(security list-keychains -d user | tr -d '"')
identity=$(security find-identity -v -p codesigning "$keychain" \
  | sed -n 's/.*"\(Developer ID Application: [^"]*\)".*/\1/p' | head -n 1)
if [ -z "$identity" ]; then
  echo "the certificate holds no Developer ID Application identity" >&2
  exit 1
fi

# The tools load a game's own code (.slash/gameplay/game.so) and plugins, which nobody signed
# with this team's ID, so the hardened runtime lets them load other teams' libraries.
cat > "$work/tools.entitlements" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>com.apple.security.cs.disable-library-validation</key>
  <true/>
</dict>
</plist>
EOF
signed=()
while IFS= read -r -d '' file; do
  if file -b "$file" | grep -q 'Mach-O'; then
    codesign --force --options runtime --timestamp --keychain "$keychain" \
      --entitlements "$work/tools.entitlements" --sign "$identity" "$file"
    codesign --verify --strict --verbose=2 "$file"
    signed+=("$file")
  fi
done < <(find "$prefix/bin" -type f -print0)
if [ "${#signed[@]}" -eq 0 ]; then
  echo "nothing to sign under $prefix/bin" >&2
  exit 1
fi

# Apple takes the signed tools in a zip and says whether they pass.
ditto -c -k --keepParent "$prefix/bin" "$work/tools.zip"
printf '%s' "$APPLE_API_KEY" | base64 --decode > "$work/AuthKey.p8"
xcrun notarytool submit "$work/tools.zip" --key "$work/AuthKey.p8" --key-id "$APPLE_API_KEY_ID" \
  --issuer "$APPLE_API_ISSUER" --wait --timeout 1h --output-format json | tee "$work/result.json"
status=$(plutil -extract status raw -o - "$work/result.json")
if [ "$status" != "Accepted" ]; then
  id=$(plutil -extract id raw -o - "$work/result.json")
  xcrun notarytool log "$id" --key "$work/AuthKey.p8" --key-id "$APPLE_API_KEY_ID" \
    --issuer "$APPLE_API_ISSUER" || true
  echo "notarization ended $status" >&2
  exit 1
fi
security delete-keychain "$keychain"
echo "signed and notarized: ${signed[*]}"
