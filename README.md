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
- **Ad styles** (`src/ai/AdStyles.*`): the model picks one word (energetic, bold, playful,
  elegant, luxury, minimal) and the app designs the polish wherever the plan left a choice
  open: transitions on every cut timed to the style, a colour look, punch-ins and a flash on
  cuts, Ken Burns on photos, text entrances and exits. Explicit choices win; switching style
  swaps the whole treatment; things the user removed stay removed.
- **Brand kit** (`src/ai/Brand.*`, the Brand button in the chat panel): colours, font, logo
  and house style, applied by the app to everything the assistant makes (or only when a
  request mentions the brand): brand colours on buttons, badges and lower thirds (with
  readable text), the brand font, the logo as a corner watermark, text-only videos on the
  brand colour.
- **Feature guards**: when a request names an effect, speed change, transition or entrance
  the plan doesn't contain, the app tells the model exactly what to add (with the cut times);
  when the target is unambiguous ("the photo", "the whole video") it applies it itself.
- **Generated media** (`src/ai/Generators.*`): ask for what you don't have and local models
  make it: images from ComfyUI (SDXL + the 8-step Lightning LoRA; product backgrounds, b-roll
  stills), voiceovers from Kokoro and instrumental music from ACE-Step 1.5. Plans list them in
  `"generate"` and use them by name (`{"media": "g1"}`); agent mode also has a
  `generate_media` tool. Files go to `~/Movies/Montage/Generated`, named after the request,
  so asking again reuses them. The chat shows a progress row per file, and Stop cancels.
  Guards add what the request asks for and the plan forgot (the voiceover says your quoted
  words exactly; music only when there's no audio file to use), drop generation nobody asked
  for, place unused results, make music dip under a voiceover, and offer to compose music
  for an ad that has none. Before an image or music job the app unloads Ollama's models (and
  ComfyUI's before music) so a 24 GB Mac doesn't swap.

  Try: *Make a 10 second ad for my candle shop with a cozy background image, a voiceover
  saying "Light up your evenings" and soft music.*
- **Media understanding** (`src/engine/Scenes.*`, `src/ai/MediaIndexer.*`): every imported video
  and photo gets a scene index in the background. Cuts are found from colour-histogram jumps;
  each shot is measured for sharpness, exposure, camera shake and faces, labelled by Apple's
  on-device image classifier, and then described by a local vision model (qwen3-vl through
  Ollama, about ten seconds a shot; the non-thinking `qwen3-vl:8b-instruct` build is preferred).
  The model sees each file's shots in the media list ("2) 3-6s blurred fractal (close-up,
  blurry)"), so it chooses footage by what it shows. Plans can name a shot instead of seconds
  (`"shot": 2` or `"moment": "dog running"`) and the app picks the sharpest, steadiest stretch
  of the right length. "The best parts" / "highlights" / "a sharper one" requests are applied
  directly. When a request names what's in a shot ("the color bars, then the forest"), the
  app uses that shot even if the model picked another one of the same file; weaker matches
  are pointed out to the model instead. A clip that has to run longer than its shot is
  slowed slightly rather than spilling into the next shot. Agent mode has a `find_moments` tool. Indexes are cached per file; work pauses
  while the assistant answers. The media panel shows progress and shot counts.
- **Smart edit** (`src/engine/Beats.*`, `Speech.*`, `src/ai/Finishing.*`): the app does the
  parts that need the media itself after a plan is built, off the UI thread.
  - *Cut to the beat* (`"beat_sync": "beats" | "bars"`): a beat tracker (spectral-flux onsets
    weighted to the kick, tempo by autocorrelation, beats by dynamic programming) moves every
    cut onto the music; text and effect moments on a cut move with it. Ad styles turn it on
    when there's music (lively styles on the beat, elegant and luxury on the bar).
  - *Captions* (`"captions": "caption" | "kinetic-caption" | ...`): whisper.cpp transcribes
    what's said on this Mac, word by word; captions are chunked at pauses and punctuation,
    timed to the words (kinetic captions reveal as they're spoken), and regenerated whenever
    the edit changes. Transcripts are cached per file range.
  - *Ducking* (`"duck": 0.3` on music, `"speech": true` on a voice): the music keeps its level
    between phrases and dips while someone talks, found from the voice's loudness.
  - *Remove background* (the `cutout` effect): Vision's foreground mask keeps the person or
    product and makes the rest transparent, over the background colour or any clip beneath.

  Requests like "cut it to the beat", "add captions", "lower the music when she talks" and
  "remove the background from the photo" are applied by the app directly; the timeline
  toolbar has the same controls (♪ Beat sync, CC Captions), and audio clips show beat ticks.

### Generated media setup

The backends are expected where `~/ai-studio` keeps them; each path can be overridden with an
environment variable (or the `generate/<key>` setting):

| Backend | Default | Override |
| --- | --- | --- |
| ComfyUI server | `http://127.0.0.1:8188`, checkout in `~/ai-studio/comfyui` | `MONTAGE_COMFY_URL`, `MONTAGE_COMFY_DIR` |
| SDXL files | `sd_xl_base_1.0.safetensors`, `sdxl_vae_fp16_fix.safetensors`, `sdxl_lightning_8step_lora.safetensors` | `MONTAGE_CHECKPOINT`, `MONTAGE_VAE`, `MONTAGE_LORA` |
| Kokoro | weights in `~/ai-studio/models/kokoro-82m`; the first Python found with `kokoro` installed (`~/ai-studio/{kokoro,envs/kokoro,tools/kokoro}/.venv`, then the animation-studio project's `envs/kokoro/.venv`) | `MONTAGE_KOKORO_PYTHON`, `MONTAGE_KOKORO_MODEL` |
| ACE-Step 1.5 | checkout and env in `~/ai-studio/ace-step`, weights in `~/ai-studio/models/ace-step-1.5` | `MONTAGE_ACE_ROOT`, `MONTAGE_ACE_PYTHON`, `MONTAGE_ACE_MODEL` |
| Output | `~/Movies/Montage/Generated` | `MONTAGE_OUTPUT_DIR` |

Montage never starts ComfyUI itself. If it isn't running, the image row says so with the
command to start it:

```bash
cd ~/ai-studio/comfyui && .venv/bin/python main.py --listen 127.0.0.1 --port 8188
```

Kokoro and ACE-Step run as subprocesses through `scripts/generate/*.py` (built into the app).
Typical times on an M4 with 24 GB: voiceover ~8 s, SDXL image 15-60 s, 10 s of music ~40 s.
`MONTAGE_FREE_MEMORY=0` keeps Ollama's and ComfyUI's models loaded.

### Measured with a local model

qwen2.5:7b on Ollama (Apple Silicon), guided mode:

| Set | Latest build | Previous build (2 runs) | Time per request |
| --- | --- | --- | --- |
| `tests/eval/cases.json` (tuning set) | 16/16 | 16/16, 15/15 | 4-35 s |
| `tests/eval/holdout.json` (6 cases) | 6/6 | 6/6, 6/6 | 4-17 s |
| `tests/eval/holdout2.json` (6 cases) | 6/6 | 6/6, 6/6 | 4-13 s |
| `tests/eval/ads.json` (ads and effects, tuning set) | 10/10 | 8/10, 4/10 (before the feature guards) | 7-80 s |
| `tests/eval/ads-holdout.json` (5 cases, never tuned against) | 5/5 | - | 10-70 s |
| `tests/eval/generate.json` (generated image, voiceover, music; skipped when a backend is down) | 2/2 | - | 10-130 s (first run), 10-20 s cached |
| `tests/eval/scenes.json` (choosing shots: best part, by content, skip blurry) | 5/5 | - | - |
| `tests/eval/smart.json` (beat sync, captions, ducking, background removal) | 5/5 | 4/5 (before the background-colour fix) | 7-50 s |

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
- Canvas ratio: Original (follows the first clip), 16:9, 9:16, 1:1, 4:5, 4:3, 3:4, 21:9
- Auto Reframe: main clips fill any shape and follow their subject (faces first, then Vision
  attention saliency, tracked across the clip and smoothed). Per clip: Auto, Fill, Fit, or Fit
  with a blurred fill. Logos and other cut-out graphics stay whole.
- Projects saved as JSON (`.montage`)
- Text and titles: presets (title, subtitle, caption, lower third, end card, and for ads
  headline, badge, price, CTA button, kinetic caption), shapes (pill, burst, tag, ribbon,
  circle, underline, rounded), shadow, gradient, size, colour, outline, box, alignment
- Animation: entrance/exit/loop motions (pop, slide, drop, spin, blur, typewriter, kinetic
  words, Ken Burns, pulse...), keyframes on position, scale, rotation and opacity with easing
- Effects on Core Image/Metal: 30+ looks and effects (cinematic, vintage, noir, golden hour,
  neon, glow, grain, VHS, glitch, RGB split, light leak, LUT `.cube` files...) on clips and text
- Effect moments (adjustment layers): flash, white-out, shake, zoom punch, glitch, blur in/out
- 19 transitions on main-track cuts (crossfade, whip, zoom, spin, glitch, push, slide, wipe, circle...)
- Speed: constant speed, freeze frames, speed ramps (ramp up/down, hero, bullet) with time-remapped audio
- MP4 export (H.264 + AAC) at canvas resolution, or every platform at once (9:16, 4:5, 1:1,
  16:9), each re-laid-out and reframed
- Scene index: shots, quality (sharp, steady, exposure, faces), on-device labels and local
  vision-model descriptions for every clip; shot counts in the media panel
- Smart edit: cut to the beat (beat ticks on music clips), auto captions from speech
  (whisper.cpp, local), music that dips while someone talks, background removal (Vision)

## Build

```bash
brew install qt ffmpeg cmake ninja
```

Captions need whisper.cpp and a model (any `ggml-*.bin`; `small` is a good default):

```bash
brew install whisper-cpp
```

```bash
mkdir -p ~/ai-studio/models/whisper && curl -L -o ~/ai-studio/models/whisper/ggml-small.bin https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin
```

`MONTAGE_WHISPER` and `MONTAGE_WHISPER_MODEL` point at another binary or model.

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
`--screenshot <png>` (render the window to a PNG, then quit) are also available. `--plan <json>`
applies an edit plan (making its `"generate"` media first), `--frame <seconds> --frame-out <png>` renders one frame, and
`--formats 9:16,1:1,16:9` with `--export` writes one file per shape, `--beats <file>`
prints a file's tempo, beats, bars and accents, and `--scenes <file>` prints its scene index
(add `--describe` to have the vision model describe each shot). `MONTAGE_BRAND_JSON`
overrides the saved brand kit for a scripted run.

Talk to the assistant without the UI (`--profile` picks the model):

```bash
build/Montage.app/Contents/MacOS/Montage --profile ollama --import tests/media/beach.mp4 --import tests/media/city.mov --agent "Make a 10 second vertical video with a title that says Hello"
```

## Tests

Generate the test media first (needs `ffmpeg`, and macOS `say` for the spoken clip):

```bash
tests/make_media.sh
```

`project_test` drives the editing model (split, move, trim, track placement, text clips,
undo groups, live slider edits, save/load) against real media files, plus beat tracking,
cutting to the beat, ducking spans and captions (skipped without whisper.cpp):

```bash
build/project_test tests/media
```

`plan_test` covers edit plans (round trip, validation, repairs), both model adapters
against a local mock server (streaming, tool calls, thinking blocks, images, JSON mode), and
generated media with fake backends (the `generate` field, binding, caching, cancel, guards):

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
| `src/ai/PlanGuards.*`, `AdStyles.*`, `Brand.*` | Deterministic guards, ad style designer, brand kit | - |
| `src/ai/Generators.*`, `scripts/generate/` | Generated images, voiceovers and music (ComfyUI, Kokoro, ACE-Step) | - |
| `src/engine/Animation.*`, `Effects.mm`, `Reframe.mm` | Motions and keyframes, Core Image effects and cutout, Auto Reframe (Vision) | VE effect / animation modules |
| `src/engine/Scenes.*`, `src/ai/MediaIndexer.*` | Scene index: shots, quality, Vision labels, vision-model descriptions | CapCut's smart scene / highlight features |
| `src/engine/Beats.*`, `Speech.*`, `src/ai/Finishing.*` | Beat tracking and cut-to-the-beat, speech spans and ducking, whisper.cpp captions | CapCut's beat / auto-caption features |
| `src/app/Agent.*` | Agent loop, guided planner, chat transcript, model profiles | - |
| `qml/` | UI: chat, media, player, inspector, timeline | `TimeLine`, `Player`, `MaterialPanel`, `FunctionPanel` QML modules |

## Known gaps and next steps

- Compositing runs on the CPU (QPainter); effects run on the GPU through Core Image. Moving
  compositing to Qt RHI shaders would put all of it on Metal the way CapCut's AGFX layer does.
- Decoding is software (multithreaded). FFmpeg's VideoToolbox hwaccel would cut CPU use for 4K.
- HDR (HLG/PQ) sources are shown without tone mapping.
- Not built yet: audio waveforms, proxies, generated video clips, cutting by transcript
  (text-based editing).
- Auto Reframe analyses a clip the first time it is drawn (a fraction of a second per clip).
- The scene index describes one frame per shot (the best stretch's middle), so action inside a
  long shot isn't described beat by beat; a 20-minute file takes a few minutes to describe.
- The background-removal mask is computed per frame (fine for stills and short clips; long
  videos preview slowly with it on).
- Shipping: build an LGPL-only FFmpeg and link it dynamically, run `macdeployqt`, then sign and notarize.
