# Montage

A desktop video editor built on the same technology stack as CapCut for
macOS: **C++20 + Qt 6 (Qt Quick / QML) + FFmpeg**, with hardware H.264 export
through VideoToolbox. See [docs/capcut-teardown.md](docs/capcut-teardown.md)
for how CapCut is put together and which parts we rebuilt on open components.

All code here is original. No CapCut code, libraries, effects or assets are used.

## Features

- Import video, audio and photos (file dialog, or drag from Finder)
- Timeline with a magnetic main track, overlay (picture-in-picture) tracks and audio tracks
- Drag clips to move them, including between tracks; drag either edge to trim; snapping to the playhead and clip edges
- Split at the playhead (⌘B), delete (⌫), undo/redo (⌘Z / ⇧⌘Z)
- Live preview with audio, frame stepping (← →), scrubbing on the ruler
- Inspector: scale, position, opacity, volume; drag the picture or scroll over it on the preview to move or scale it
- Canvas ratio: Original (follows the first clip), 16:9, 9:16, 1:1, 4:3, 3:4, 21:9
- Projects saved as JSON (`.montage`)
- MP4 export (H.264 + AAC) at canvas resolution

## Build

```bash
brew install qt ffmpeg cmake ninja
```

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
```

```bash
cmake --build build
```

```bash
open build/Montage.app
```

## Command line

The app binary also works headless, for scripted renders and checks:

```bash
build/Montage.app/Contents/MacOS/Montage --import a.mp4 --import b.mov --import song.mp3 --overlay logo.png --save demo.montage --export demo.mp4
```

`--open <project>`, `--seek <seconds>`, `--play`, `--select-first` and
`--screenshot <png>` (render the window to a PNG, then quit) are also available.

## Tests

`project_test` drives the editing model (split, move, trim, track placement,
undo/redo, live slider edits, save/load) against real media files:

```bash
build/project_test /path/to/media
```

The media folder needs `clipA.mp4`, `clipB.mov`, `music.mp3` and `photo.png`.

## Layout

| Path | Role | CapCut counterpart |
| --- | --- | --- |
| `src/model/Draft.*` | Project document: materials, tracks, segments, JSON | `lvve::Draft` in `libvideoeditor` |
| `src/engine/VideoDecoder.*`, `AudioDecoder.*` | Random-access FFmpeg decoding, rotation, resampling | FFmpeg fork + VE decoders |
| `src/engine/Renderer.*` | Composites a frame / mixes audio at any time | `TEEditorGraph` |
| `src/engine/PreviewEngine.*` | Playback thread, clock, audio output | `TEPreviewGraph` |
| `src/engine/ExportJob.*` | Offline render to MP4 | `TECompileGraph` |
| `src/app/Project.*` | Edit API for QML, undo/redo | `NewVEWrapper` / draft editing |
| `src/app/VideoSurface.*` | Shows preview frames in the scene graph | CapCut's `QSGSimpleTextureNode` player |
| `qml/` | UI: media, player, inspector, timeline | `TimeLine`, `Player`, `MaterialPanel`, `FunctionPanel` QML modules |

## Known gaps and next steps

- Compositing runs on the CPU (QPainter). Moving it to Qt RHI shaders would put it
  on the GPU (Metal) the way CapCut's AGFX layer does, and enable effects and transitions.
- Decoding is software (multithreaded). FFmpeg's VideoToolbox hwaccel would cut CPU use for 4K.
- HDR (HLG/PQ) sources are shown without tone mapping.
- Not built yet: text and titles, transitions, speed changes, keyframes, audio waveforms, proxies.
- Shipping: build an LGPL-only FFmpeg and link it dynamically, run `macdeployqt`, then sign and notarize.
