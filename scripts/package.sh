#!/bin/sh
# Builds a self-contained Montage.app and Montage.dmg in dist/.
#   - Release build against the LGPL FFmpeg in deps/ (scripts/build-deps.sh)
#   - Qt frameworks, QML modules and plugins copied in by macdeployqt
#   - whisper-cli bundled for captions and text-based editing
#   - icon, version and the minimum macOS (what the bundled frameworks need) in Info.plist
#   - signed ad hoc (runs on this Mac and, after "Open" in the Finder, on
#     others). For distribution, sign and notarize with scripts/notarize.sh.
# Usage: scripts/package.sh [version]
set -e
root="$(cd "$(dirname "$0")/.." && pwd)"
version="${1:-$(sed -n 's/^project(Montage VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")}"
qt="$(brew --prefix qt)"
build="$root/build-release"
dist="$root/dist"
app="$dist/Montage.app"

[ -f "$root/deps/ffmpeg/lib/libavcodec.dylib" ] && [ -x "$root/deps/whisper/bin/whisper-cli" ] || "$root/scripts/build-deps.sh"

echo "== build $version"
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$qt" \
  -DFFMPEG_ROOT="$root/deps/ffmpeg" -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 >/dev/null
cmake --build "$build" --target Montage

echo "== bundle"
rm -rf "$dist" && mkdir -p "$dist"
cp -R "$build/Montage.app" "$app"
cp "$root/deps/whisper/bin/whisper-cli" "$app/Contents/MacOS/"

# Icon (drawn by the app itself) and Info.plist details.
iconset="$dist/Montage.iconset"
mkdir -p "$iconset"
"$app/Contents/MacOS/Montage" --render-icon "$dist/icon-1024.png"
for size in 16 32 128 256 512; do
  sips -z $size $size "$dist/icon-1024.png" --out "$iconset/icon_${size}x${size}.png" >/dev/null
  double=$((size * 2))
  sips -z $double $double "$dist/icon-1024.png" --out "$iconset/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$iconset" -o "$app/Contents/Resources/Montage.icns"
rm -rf "$iconset" "$dist/icon-1024.png"
plist="$app/Contents/Info.plist"
chmod u+w "$plist"
set_key() { /usr/libexec/PlistBuddy -c "Delete :$1" "$plist" 2>/dev/null || true; /usr/libexec/PlistBuddy -c "Add :$1 $2 $3" "$plist"; }
set_key CFBundleIconFile string Montage
set_key CFBundleDisplayName string Montage
set_key CFBundleShortVersionString string "$version"
set_key CFBundleVersion string "$version"
set_key LSMinimumSystemVersion string 14.0  # raised below to what the bundled frameworks need
set_key LSApplicationCategoryType string public.app-category.video
set_key NSHighResolutionCapable bool true
set_key NSHumanReadableCopyright string "Montage. Uses Qt (LGPL), FFmpeg (LGPL) and whisper.cpp (MIT)."

mkdir -p "$app/Contents/Resources/Licenses"
cp "$root/packaging/THIRD_PARTY_NOTICES.txt" "$app/Contents/Resources/Licenses/"

echo "== qt"
"$qt/bin/macdeployqt" "$app" -qmldir="$root/qml" -always-overwrite -verbose=1

echo "== prune plugins Montage doesn't use (their frameworks aren't there)"
missing_deps() {  # prints @rpath dependencies of $1 that the bundle doesn't have
  otool -L "$1" | tail -n +2 | awk '{print $1}' | grep '^@rpath/' | while read -r dep; do
    [ -e "$app/Contents/Frameworks/${dep#@rpath/}" ] || echo "${dep#@rpath/}"
  done
}
find "$app/Contents/PlugIns" -name "*.dylib" | while read -r f; do
  if [ -n "$(missing_deps "$f")" ]; then echo "  removed ${f#$app/Contents/}"; rm -f "$f"; fi
done

# ...and the links QML import folders kept to them.
find -L "$app" -type l -exec rm -f {} \;

echo "== check: nothing outside the bundle but macOS itself"
find "$app" -type f \( -perm -u+x -o -name "*.dylib" \) | while read -r f; do
  file "$f" | grep -q "Mach-O" || continue
  if otool -L "$f" | tail -n +2 | awk '{print $1}' | grep -v -E '^(@rpath|@executable_path|@loader_path|/System/|/usr/lib/)' | grep -q .; then
    echo "  outside the bundle: $f"
    otool -L "$f" | tail -n +2 | awk '{print $1}' | grep -v -E '^(@rpath|@executable_path|@loader_path|/System/|/usr/lib/)' | sed 's/^/      /'
  fi
  for dep in $(missing_deps "$f"); do echo "  missing in the bundle: $dep (needed by ${f#$app/})"; done
done

# The oldest macOS the bundle runs on is the newest minimum among its binaries
# (Homebrew's Qt is built for the macOS it was compiled on).
minos="$(find "$app" -type f \( -perm -u+x -o -name "*.dylib" \) -exec sh -c 'file "$1" | grep -q Mach-O && otool -l "$1" | awk "/LC_BUILD_VERSION/{f=1} f&&/minos/{print \$2; exit}"' _ {} \; | sort -t. -k1,1n -k2,2n | tail -1)"
if [ -n "$minos" ]; then
  /usr/libexec/PlistBuddy -c "Set :LSMinimumSystemVersion $minos" "$plist"
  echo "  runs on macOS $minos or later"
fi

echo "== sign (ad hoc)"
codesign --force --deep --sign - "$app"
codesign --verify --deep --strict "$app" && echo "  signature ok"

echo "== dmg"
stage="$dist/dmg"
mkdir -p "$stage"
cp -R "$app" "$stage/"
ln -s /Applications "$stage/Applications"
hdiutil create -volname "Montage" -srcfolder "$stage" -ov -format UDZO "$dist/Montage-$version.dmg" >/dev/null
rm -rf "$stage"
du -sh "$app" "$dist/Montage-$version.dmg"
echo "done: $dist/Montage-$version.dmg"
