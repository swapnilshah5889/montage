# Montage

A desktop video editor built on the same technology stack as CapCut for
macOS: **C++20 + Qt 6 (Qt Quick / QML) + FFmpeg**, with hardware H.264 export
through VideoToolbox. See [docs/capcut-teardown.md](docs/capcut-teardown.md)
for how CapCut is put together and which parts we rebuilt on open components.

All code here is original. No CapCut code, libraries, effects or assets are used.

Montage is AI-native: you describe the video you want in a chat panel and an
assistant edits the timeline for you. It works with any model: Claude, OpenAI,
DeepSeek, OpenRouter, or local models through Ollama or LM Studio.

## Assistant

- **Any model.** One adapter speaks the OpenAI-compatible API (OpenAI, DeepSeek, OpenRouter,
  Groq, Ollama, LM Studio, llama.cpp, vLLM); a native adapter speaks Claude's Messages API.
  Add models under the ⚙ button in the chat panel; API keys are stored in the macOS Keychain
  (or read from `ANTHROPIC_API_KEY`, `OPENAI_API_KEY`, `DEEPSEEK_API_KEY`, `OPENROUTER_API_KEY`).
- **Agent mode** (capable models): the model uses the editor's tools: read the project,
  apply an edit plan, look at frames of the media, render frames of the result to check its
  work, find and import files, export.
- **Guided mode** (small/local models, the default for local endpoints): the model answers
  each request with one JSON edit plan, constrained to a schema at decode time. The app
  validates it, repairs common small-model mistakes, checks it against the request (length,
  aspect, requested text and music, clips dropped by accident) and asks the model to fix
  anything that's off, at most twice.
- **Edit plans** (`src/ai/EditPlan.*`) describe the whole timeline in seconds with named
  positions; both modes use them. Every request is one undo step.

### Measured with a local model

qwen2.5:7b on Ollama (Apple Silicon), guided mode:

| Set | Latest build | Previous build (2 runs) | Time per request |
| --- | --- | --- | --- |
| `tests/eval/cases.json` (tuning set) | 16/16 | 15/15, 15/15 | 4-35 s |
| `tests/eval/holdout.json` (6 cases) | 6/6 | 6/6, 6/6 | 4-17 s |
| `tests/eval/holdout2.json` (6 cases) | 6/6 | 6/6, 6/6 | 4-13 s |

Both hold-out sets were written before tuning against them, but each was later read to
debug a failure (a crash, then the logo cases), so neither is fully blind any more. Add a
fresh set before trusting a big prompt change. Most of the reliability comes from the app's
deterministic guards (`src/ai/PlanGuards.*` and the checks in `Agent::planFeedback`), not
from the model: keeping parts of the timeline the request didn't mention, hitting a requested
length exactly, repairing file-range mistakes, applying a background colour named in the request,
and asking for a fix when a named file, a quoted text, "both"/"all" clips or a requested aspect
is missing. Small models often reply that they did something their plan doesn't contain; these
checks are what catch that.

Reproduce:

```bash
tests/make_media.sh
```

```bash
build/Montage.app/Contents/MacOS/Montage --eval tests/eval/cases.json --media tests/media --profile ollama
```

The Claude and OpenAI-compatible adapters are covered by `plan_test` against a local mock
server; agent mode hasn't been scored against a live hosted model yet.

## Features

- Import video, audio and photos (file dialog, or drag from Finder)
- Timeline with a magnetic main track, overlay (picture-in-picture) tracks and audio tracks
- Drag clips to move them, including between tracks; drag either edge to trim; snapping to the playhead and clip edges
- Split at the playhead (⌘B), delete (⌫), undo/redo (⌘Z / ⇧⌘Z)
- Live preview with audio, frame stepping (← →), scrubbing on the ruler
- Inspector: scale, position, opacity, volume; drag the picture or scroll over it on the preview to move or scale it
- Canvas ratio: Original (follows the first clip), 16:9, 9:16, 1:1, 4:3, 3:4, 21:9
- Projects saved as JSON (`.montage`)
- Text and titles: presets (title, subtitle, caption, lower third, end card), size, colour, outline, box, alignment
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

`--open <project>`, `--seek <seconds>`, `--play`, `--select-first`, `--text <preset>:<text>` and
`--screenshot <png>` (render the window to a PNG, then quit) are also available.

Talk to the assistant without the UI (`--profile` picks the model):

```bash
build/Montage.app/Contents/MacOS/Montage --profile ollama --import tests/media/beach.mp4 --import tests/media/city.mov --agent "Make a 10 second vertical video with a title that says Hello"
```

## Tests

Generate the test media first (needs `ffmpeg`):

```bash
tests/make_media.sh
```

`project_test` drives the editing model (split, move, trim, track placement, text clips,
undo groups, live slider edits, save/load) against real media files:

```bash
build/project_test tests/media
```

`plan_test` covers edit plans (round trip, validation, repairs) and both model adapters
against a local mock server (streaming, tool calls, thinking blocks, images, JSON mode):

```bash
build/plan_test
```

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
| `src/ai/ChatProvider.*` | Model-agnostic chat: OpenAI-compatible and Anthropic adapters | - |
| `src/ai/EditPlan.*` | Timeline ⇄ JSON edit plan, validation, repairs | - |
| `src/ai/EditorTools.*`, `Prompts.*` | Agent tools and system prompts | `Agent` / `SmartAssistant` QML modules |
| `src/app/Agent.*` | Agent loop, guided planner, chat transcript, model profiles | - |
| `qml/` | UI: chat, media, player, inspector, timeline | `TimeLine`, `Player`, `MaterialPanel`, `FunctionPanel` QML modules |

## Known gaps and next steps

- Compositing runs on the CPU (QPainter). Moving it to Qt RHI shaders would put it
  on the GPU (Metal) the way CapCut's AGFX layer does, and enable effects and transitions.
- Decoding is software (multithreaded). FFmpeg's VideoToolbox hwaccel would cut CPU use for 4K.
- HDR (HLG/PQ) sources are shown without tone mapping.
- Not built yet: transitions, speed changes, keyframes, audio waveforms, proxies.
- Assistant: no speech transcription yet (captions from speech, cutting by what's said);
  guided mode sees media only through names and lengths unless the model can read images.
- Shipping: build an LGPL-only FFmpeg and link it dynamically, run `macdeployqt`, then sign and notarize.
