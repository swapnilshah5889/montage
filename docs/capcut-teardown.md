# CapCut for macOS: technology teardown

Target: CapCut 9.5.0 (build 9.5.0.4590), `com.lemon.lvoverseas`, official DMG from
`capcutstatic.com` (SHA-256 `93306fb1…7e71f`, matches the Homebrew cask).
Method: static inspection only (REA 6.7.0 `inspect-plist` / `inspect-macho`, `nm`,
`strings`, `otool`). CapCut was never installed or run.

## TL;DR

CapCut desktop is a **C++ application with a Qt 6 / QML user interface**, sitting
on ByteDance's proprietary **VE (video editor) engine**, which itself is a
**graph-based renderer on top of FFmpeg** and a GPU abstraction layer that targets
**Metal, Vulkan and OpenGL ES (via ANGLE)**. Embedded Chromium (CEF) and
ByteDance's Lynx engine are used only for web-style side pages
(subscriptions, credits, message center, template publishing).

| Layer | What CapCut ships | Evidence |
| --- | --- | --- |
| Language | C++ (libc++), Objective-C++ / Swift glue for macOS | `libc++.1.dylib`, `libswift*.dylib`, ObjC classes in `libcccreator` |
| UI framework | **Qt 6.2.2**, Qt Quick + QML, Quick Controls 2, Qt5Compat.GraphicalEffects, Lottie (QtBodymovin) | `QtCore` version string, `PlugIns/quick/*`, `qt.conf` |
| App code | `libVECreator.dylib` (1.1 GB) – ~30 QML modules, ~3,000 QML files compiled in via `qrc:/` | `qml_register_types_TimeLine`, `_Player`, `_Export` … |
| Document model | `libvideoeditor.dylib` – namespace `lvve` (Draft / Track / Segment / Material / Keyframe), JSON serialized | 32.5k exports, 1,126 `lvve::to_json` overloads |
| Edit / render engine | `libcccreator.dylib` – namespace `vesdk` + `core::TE*` graphs + `AmazingEngine` effects | `TEEditorGraph`, `TEPreviewGraph`, `TECompileGraph`, `VEEditorImpl` |
| GPU | `libAGFX.dylib` (Metal / Vulkan / GLES abstraction), ANGLE (`libEGL`, `libGLESv2`), precompiled Metal shaders (`VEMetalBinary_Mac.bundle`) | strings in `libAGFX` |
| Codecs / IO | **FFmpeg 7.0 fork** (`libavcodec` 61.3.100, "FFmpeg version 1.0.0-1a403fa6a9"), dav1d (AV1), LAME (MP3), ByteVC1 (ByteDance HEVC decoder), VideoToolbox | `otool -L`, version strings |
| AI / ML | `libbytenn` (NN inference), `liblens` (enhancement, color match), `libsamicore` (audio, separation), `libspeechsdk` (ASR/TTS), `libfastcv`, Ceres solver (tracking), `libdeepagents_capi` (AI agent), `Resources/models` | exported namespaces `lensii::`, `ceres::` |
| Scripted effects | LumiGene JS runtime (`libLumiGeneRuntime`, `Resources/lumi_js_resources`) | resource tree |
| Web UI | CEF 121 (Chrome 121.0.6167.86) + 4 helper apps, Lynx (`libLynx`, `Resources/lynx_config/*`) | framework Info.plist |
| Networking | Cronet (`libsscronet`), BoringSSL, TTNet downloader | linkage |
| Storage | MMKV (key-value), SQLite (`QtSql`), drafts as JSON files (`draft_content.json`, `draft_meta_info.json`) | strings in `libvideoeditor` |
| Updates | Sparkle 2.7.0 | framework Info.plist |
| Other | Crypto++, protobuf, ICU 72, spdlog, Boost.DI, C2PA content-credentials SDK | exported namespaces |

## Architecture

```
┌───────────────────────────────────────────────────────────────────┐
│  CapCut (447 KB launcher)                                         │
│   └─ libVECreator  ── Qt 6 QML app                                │
│        HomeScene · TimeLine · Player · MaterialPanel ·            │
│        FunctionPanel (inspector) · TextEditor · Export · Cover …  │
│        CEF / Lynx pages for store, account, publishing            │
├───────────────────────────────────────────────────────────────────┤
│  libvideoeditor  (lvve)                                           │
│   Draft ─┬─ CanvasConfig                                          │
│          ├─ Track[]  (main video, above-main video/effect/text,   │
│          │            under-main audio)                           │
│          │    └─ Segment[]  target_timerange · source_timerange · │
│          │                  material_id · extra_material_refs ·   │
│          │                  keyframe_refs · render_index          │
│          ├─ Materials{ videos, audios, texts, stickers, effects,  │
│          │            transitions, speeds, masks, canvases … }    │
│          └─ Keyframes                                             │
│   DraftStore (persistence) · DraftQuery · EditResult (undo/redo)  │
│   NewVEWrapper → drives the engine below                          │
├───────────────────────────────────────────────────────────────────┤
│  libcccreator  (vesdk / TE core)                                  │
│   TEMediaEditorContext → TEEditorGraph                            │
│        ├─ TEPreviewGraph  (real-time playback)                    │
│        └─ TECompileGraph  (export / "compile")                    │
│   AmazingEngine segments: Video · Text · Sticker · Feature        │
├───────────────────────────────────────────────────────────────────┤
│  FFmpeg fork · VideoToolbox · AGFX (Metal/Vulkan/GLES) · ANGLE    │
│  bytenn / lens / samicore / speechsdk (ML)                        │
└───────────────────────────────────────────────────────────────────┘
```

Key design choices worth copying (as ideas, not code):

1. **Draft-as-JSON document.** Media is "material", and a placement on the timeline is a
   "segment" that points at a material by id with two time ranges: where it sits on
   the timeline and which part of the source it plays. Everything else (speed, masks,
   adjustments, animations) is an extra material referenced from the segment.
2. **One main track plus overlay tracks.** The main video track is magnetic; other
   tracks are "above main" (overlays, text, stickers, effects) or "under main" (audio).
3. **One graph, two consumers.** The same edit graph feeds a real-time preview graph
   and an offline export ("compile") graph, so preview and export match.
4. **The QML UI never touches pixels.** The player draws GPU frames into the Qt scene
   graph (`QSGSimpleTextureNode`) and the UI only issues edits to the draft.

## What we can and cannot reuse

Everything in the "proprietary" column is ByteDance code and stays out of our product.
We rebuild those layers ourselves on open components.

| CapCut component | Ours |
| --- | --- |
| Qt 6 / QML / Quick Controls 2 (LGPL) | **Same** – Qt 6.12 |
| C++ / Objective-C++ | **Same** – C++20 |
| FFmpeg (LGPL fork) | **Same** – upstream FFmpeg |
| VideoToolbox | **Same** (through FFmpeg hwaccel) |
| Sparkle updater (MIT) | **Same**, when we ship |
| CEF (BSD) / Lynx (Apache-2.0, open-sourced 2025) | Optional, later, for web panels |
| `lvve` draft model (proprietary) | Our own JSON draft model, same concepts |
| `vesdk` / TE graphs / AmazingEngine (proprietary) | Our own engine: FFmpeg decode → compositor → encoder |
| AGFX (proprietary GPU layer) | Qt RHI (Metal / Vulkan / D3D / GL) |
| bytenn / lens / samicore / speechsdk (proprietary ML) | ONNX Runtime / Core ML, whisper.cpp for captions |
| Effect packs, templates, fonts, models, UI assets | Not reused – licensed or own content only |

Licensing note: Homebrew's FFmpeg build enables GPL components (x264, x265). That's
fine for development. A closed-source release needs an LGPL-only FFmpeg build that is
dynamically linked, the same way CapCut ships it, and Qt also needs dynamic linking
under the LGPL.
