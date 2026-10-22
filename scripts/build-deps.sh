#!/bin/sh
# Builds the third-party pieces Montage ships inside its app bundle, into deps/:
#   - FFmpeg as LGPL shared libraries: only FFmpeg's own decoders and muxers plus
#     Apple's VideoToolbox/AudioToolbox (no GPL parts such as x264), so the app
#     can be distributed. Montage encodes with h264_videotoolbox and AAC.
#   - whisper.cpp's whisper-cli as one self-contained binary (Metal, static libs)
#     for local captions and text-based editing.
# Usage: scripts/build-deps.sh   (about ten minutes on Apple Silicon)
set -e
root="$(cd "$(dirname "$0")/.." && pwd)"
deps="$root/deps"
src="$deps/src"
mkdir -p "$src"
jobs="$(sysctl -n hw.ncpu)"

FFMPEG_VERSION="${FFMPEG_VERSION:-9.0.2}"
WHISPER_VERSION="${WHISPER_VERSION:-1.9.5}"

# ---------------------------------------------------------------- FFmpeg (LGPL)
if [ ! -f "$deps/ffmpeg/lib/libavcodec.dylib" ]; then
  cd "$src"
  [ -f "ffmpeg-$FFMPEG_VERSION.tar.xz" ] || curl -fL -o "ffmpeg-$FFMPEG_VERSION.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
  rm -rf "ffmpeg-$FFMPEG_VERSION" && tar xf "ffmpeg-$FFMPEG_VERSION.tar.xz"
  cd "ffmpeg-$FFMPEG_VERSION"
  ./configure --prefix="$deps/ffmpeg" \
    --disable-gpl --disable-nonfree --disable-version3 \
    --enable-shared --disable-static --enable-pic \
    --disable-programs --disable-doc --disable-debug \
    --disable-avdevice --disable-avfilter --disable-network \
    --enable-videotoolbox --enable-audiotoolbox \
    --install-name-dir='@rpath' \
    --extra-cflags="-mmacosx-version-min=14.0" --extra-ldflags="-mmacosx-version-min=14.0"
  make -j"$jobs"
  make install
  echo "FFmpeg $FFMPEG_VERSION (LGPL) installed in $deps/ffmpeg"
fi

# ---------------------------------------------------------------- whisper.cpp
if [ ! -x "$deps/whisper/bin/whisper-cli" ]; then
  cd "$src"
  [ -f "whisper.cpp-$WHISPER_VERSION.tar.gz" ] || curl -fL -o "whisper.cpp-$WHISPER_VERSION.tar.gz" "https://github.com/ggml-org/whisper.cpp/archive/refs/tags/v$WHISPER_VERSION.tar.gz"
  rm -rf "whisper.cpp-$WHISPER_VERSION" && tar xf "whisper.cpp-$WHISPER_VERSION.tar.gz"
  cd "whisper.cpp-$WHISPER_VERSION"
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DGGML_METAL=ON -DGGML_METAL_EMBED_LIBRARY=ON \
    -DWHISPER_BUILD_TESTS=OFF -DWHISPER_BUILD_EXAMPLES=ON -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0
  cmake --build build -j"$jobs" --target whisper-cli
  mkdir -p "$deps/whisper/bin"
  cp build/bin/whisper-cli "$deps/whisper/bin/"
  echo "whisper-cli $WHISPER_VERSION installed in $deps/whisper/bin"
fi
echo "deps ready in $deps"
