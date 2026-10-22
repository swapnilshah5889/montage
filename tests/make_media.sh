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
# Four shots, cut every 3 s, for the scene index: sharp and steady, blurred,
# shaky (jittering crop), and a clean gradient.
f -f lavfi -i "testsrc2=size=1280x720:rate=30:duration=3" \
  -f lavfi -i "mandelbrot=size=1280x720:rate=30" \
  -f lavfi -i "smptehdbars=size=1360x800:rate=30:duration=3" \
  -f lavfi -i "gradients=size=1280x720:rate=30:duration=3:speed=0.02" \
  -f lavfi -i "sine=frequency=500:duration=12" \
  -filter_complex "[1]trim=duration=3,boxblur=18[blur];[2]crop=1280:720:'40+38*sin(n*1.9)':'40+38*cos(n*2.7)'[shake];[0][blur][shake][3]concat=n=4:v=1:a=0,format=yuv420p[v]" \
  -map "[v]" -map 4:a -c:v libx264 -c:a aac -shortest "$out/shots.mp4"
# 120 BPM drum loop (kick on every beat, hats in between) for beat sync
f -f lavfi -i "aevalsrc='0.9*sin(2*PI*55*t)*exp(-25*mod(t,0.5)) + 0.25*(random(0)-0.5)*exp(-60*mod(t+0.25,0.5))':s=44100:d=30" \
  -ac 2 -c:a libmp3lame -q:a 2 "$out/beat.mp3"
# Spoken words for captions and ducking (macOS text to speech)
if command -v say >/dev/null 2>&1; then
  say -o "$out/voiceover.aiff" "Fresh bread every morning. Order yours today."
  f -i "$out/voiceover.aiff" -ar 44100 -ac 1 "$out/voiceover.wav"
  rm -f "$out/voiceover.aiff"
  # A talking head with fillers and long pauses, for text-based editing
  say -o "$out/talk.aiff" "Hi, I'm Sam. Um, our bread is, uh, baked fresh every morning. [[slnc 1500]] We use only organic flour. Uh, and honestly, um, it tastes amazing. [[slnc 1200]] Order online today and get free delivery."
  f -f lavfi -i "testsrc2=size=1280x720:rate=30" -i "$out/talk.aiff" -map 0:v -map 1:a -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "$out/talk.mp4"
  rm -f "$out/talk.aiff"
fi

# Legacy names used by project_test
f -f lavfi -i "testsrc2=size=1920x1080:rate=30:duration=4" -f lavfi -i "sine=frequency=440:duration=4" \
  -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "$out/clipA.mp4"
f -f lavfi -i "smptehdbars=size=1280x720:rate=25:duration=3" -f lavfi -i "sine=frequency=880:duration=3" \
  -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "$out/clipB.mov"
f -f lavfi -i "sine=frequency=330:duration=8" -c:a libmp3lame "$out/music.mp3"
f -f lavfi -i "mandelbrot=size=1200x800" -frames:v 1 "$out/photo.png"
echo "media written to $out"
