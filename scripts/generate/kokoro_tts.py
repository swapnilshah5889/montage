"""Voiceover for Montage with Kokoro-82M. Montage runs this with the Python of an env that has
kokoro installed (see GeneratorConfig in src/ai/Generators.h):

    python kokoro_tts.py --model-dir ~/ai-studio/models/kokoro-82m --voice af_heart --out vo.wav "Hello!"

Runs offline from the local weights. Prints "PROGRESS <0-1> <note>" lines while it works and one
JSON line at the end: {"file": ..., "seconds": ...} or {"error": ...}.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

# Must be set before torch is imported: ops MPS lacks fall back to the CPU instead of erroring.
os.environ.setdefault("PYTORCH_ENABLE_MPS_FALLBACK", "1")
os.environ.setdefault("HF_HUB_OFFLINE", "1")

REPO_ID = "hexgrad/Kokoro-82M"
SAMPLE_RATE = 24_000


def progress(value: float, note: str) -> None:
    print(f"PROGRESS {value:.2f} {note}", flush=True)


def fail(message: str) -> int:
    print(json.dumps({"error": message}), flush=True)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("text")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--model-dir", type=Path, required=True)
    parser.add_argument("--voice", default="af_heart")
    parser.add_argument("--speed", type=float, default=1.0)
    parser.add_argument("--device", choices=["cpu", "mps"], default="cpu")  # CPU is as fast for 82M
    args = parser.parse_args()

    model_dir = args.model_dir.expanduser()
    voice = model_dir / "voices" / f"{args.voice}.pt"
    for needed in (model_dir / "config.json", model_dir / "kokoro-v1_0.pth", voice):
        if not needed.exists():
            return fail(f"{needed} is missing")

    progress(0.05, "Loading the voice model")
    import soundfile as sf
    import torch
    from kokoro import KModel, KPipeline

    model = KModel(repo_id=REPO_ID, config=str(model_dir / "config.json"), model=str(model_dir / "kokoro-v1_0.pth"))
    model = model.to(args.device).eval()
    lang = "b" if args.voice.startswith("b") else "a"  # voice prefix: a = US English, b = UK
    pipeline = KPipeline(lang_code=lang, repo_id=REPO_ID, model=model)

    progress(0.6, "Speaking")
    chunks = [r.audio for r in pipeline(args.text, voice=str(voice), speed=args.speed) if r.audio is not None]
    if not chunks:
        return fail("Kokoro produced no audio for this text")
    audio = torch.cat(chunks).cpu().numpy()
    peak = float(abs(audio).max())
    if peak > 0:
        audio = audio * (0.89 / peak)  # -1 dBFS: voiceovers sit on top of music
    args.out.parent.mkdir(parents=True, exist_ok=True)
    sf.write(str(args.out), audio, SAMPLE_RATE)
    print(json.dumps({"file": str(args.out), "seconds": round(len(audio) / SAMPLE_RATE, 2)}), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
