#!/bin/sh
# Signs dist/Montage.app with a Developer ID and the hardened runtime, has
# Apple notarize it, and staples the tickets, so it opens on any Mac without
# a Gatekeeper warning. Run scripts/package.sh first.
#
# One-time setup (needs an Apple Developer account):
#   1. Install a "Developer ID Application" certificate in the login keychain
#      (Xcode > Settings > Accounts > Manage Certificates, or developer.apple.com).
#   2. Save notarization credentials under a profile name:
#        xcrun notarytool store-credentials montage --apple-id you@example.com --team-id TEAMID
#      (it asks for an app-specific password from account.apple.com).
#
# Usage: scripts/notarize.sh "Developer ID Application: Your Name (TEAMID)" [profile]
#        scripts/notarize.sh -      # ad hoc + hardened runtime, no notarization:
#                                   # checks the app still runs with the runtime on
set -e
root="$(cd "$(dirname "$0")/.." && pwd)"
identity="$1"
profile="${2:-montage}"
dist="$root/dist"
app="$dist/Montage.app"
entitlements="$root/packaging/Montage.entitlements"
[ -n "$identity" ] || { sed -n '2,17p' "$0"; exit 1; }
[ -d "$app" ] || { echo "no $app: run scripts/package.sh first"; exit 1; }
version="$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$app/Contents/Info.plist")"
dmg="$dist/Montage-$version.dmg"
timestamp="--timestamp"
if [ "$identity" = "-" ]; then
  # Ad hoc signatures have no Team ID, so library validation would refuse the
  # bundled frameworks; a Developer ID signs them all with the same team.
  timestamp=""
  cp "$entitlements" "$dist/check.entitlements"
  entitlements="$dist/check.entitlements"
  /usr/libexec/PlistBuddy -c "Add :com.apple.security.cs.disable-library-validation bool true" "$entitlements"
fi

sign() { codesign --force --options runtime $timestamp --sign "$identity" "$@"; }

echo "== sign inside out with $identity"
# Loose libraries and plugins first, then the frameworks, the helper tool, the app.
find "$app/Contents" -type f -name "*.dylib" | while read -r f; do sign "$f"; done
find "$app/Contents/Frameworks" -maxdepth 1 -name "*.framework" | while read -r f; do sign "$f"; done
sign "$app/Contents/MacOS/whisper-cli"
sign --entitlements "$entitlements" "$app"
codesign --verify --deep --strict --verbose=1 "$app"

if [ "$identity" = "-" ]; then
  echo "== hardened-runtime check (ad hoc, not notarized)"
  # Opens the window (QML and all) for a moment, renders it and quits.
  "$app/Contents/MacOS/Montage" --screenshot "$dist/runtime-check.png" --ui-wait 2 >/dev/null
  rm -f "$entitlements"
  [ -s "$dist/runtime-check.png" ] && rm -f "$dist/runtime-check.png" && echo "  app runs with the hardened runtime"
  exit 0
fi

notarize() {  # submits $1 and waits; prints Apple's log when it's rejected
  out="$(xcrun notarytool submit "$1" --keychain-profile "$profile" --wait 2>&1)" || true
  echo "$out"
  echo "$out" | grep -q "status: Accepted" && return 0
  id="$(echo "$out" | awk '/^  id:/{print $2; exit}')"
  [ -n "$id" ] && xcrun notarytool log "$id" --keychain-profile "$profile"
  exit 1
}

echo "== notarize the app"
zip="$dist/Montage-notarize.zip"
ditto -c -k --keepParent "$app" "$zip"
notarize "$zip"
rm -f "$zip"
xcrun stapler staple "$app"

echo "== dmg"
stage="$dist/dmg"
rm -rf "$stage" "$dmg" && mkdir -p "$stage"
cp -R "$app" "$stage/"
ln -s /Applications "$stage/Applications"
hdiutil create -volname "Montage" -srcfolder "$stage" -ov -format UDZO "$dmg" >/dev/null
rm -rf "$stage"
codesign --force $timestamp --sign "$identity" "$dmg"

echo "== notarize the dmg"
notarize "$dmg"
xcrun stapler staple "$dmg"
spctl --assess --type open --context context:primary-signature --verbose "$dmg"
spctl --assess --type execute --verbose "$app"
echo "done: $dmg is signed, notarized and stapled"
