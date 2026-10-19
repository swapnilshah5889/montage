#!/bin/sh
# Generates the synthetic media used by tests and the assistant eval.
# Usage: tests/make_media.sh [output dir]   (default: tests/media)
set -e
out="${1:-$(dirname "$0")/media}"
mkdir -p "$out"
f() { ffmpeg -hide_banner -loglevel error -y "$@"; }

f -f lavfi -i "testsrc2=size=1920x1080:rate=30:duration=12" -f lavfi -i "sine=frequency=440:duration=12" \
  -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "$out/beach.mp4"
f -f lavfi -i "smptehdbars=size=1920x1080:rate=30:duration=8" -f lavfi -i "sine=frequency=660:duration=8" \
  -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "$out/city.mov"
f -f lavfi -i "mandelbrot=size=1280x720:rate=30" -f lavfi -i "sine=frequency=550:duration=10" -t 10 \
  -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "$out/forest.mp4"
f -f lavfi -i "sine=frequency=330:duration=60" -c:a libmp3lame "$out/song.mp3"
f -f lavfi -i "color=c=0x1fd1c1:size=600x600:duration=1" -frames:v 1 "$out/logo.png"

# Legacy names used by project_test
f -f lavfi -i "testsrc2=size=1920x1080:rate=30:duration=4" -f lavfi -i "sine=frequency=440:duration=4" \
  -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "$out/clipA.mp4"
f -f lavfi -i "smptehdbars=size=1280x720:rate=25:duration=3" -f lavfi -i "sine=frequency=880:duration=3" \
  -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "$out/clipB.mov"
f -f lavfi -i "sine=frequency=330:duration=8" -c:a libmp3lame "$out/music.mp3"
f -f lavfi -i "mandelbrot=size=1200x800" -frames:v 1 "$out/photo.png"
echo "media written to $out"
